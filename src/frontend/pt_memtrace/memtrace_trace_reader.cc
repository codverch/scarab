/* Copyright 2020 University of California Santa Cruz
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/***************************************************************************************
 * File         : frontend/pt_memtrace/memtrace_trace_reader.cc
 * Author       : Heiner Litz
 * Date         : 05/15/2020
 * Description  :
 ***************************************************************************************/

#include "frontend/pt_memtrace/memtrace_trace_reader.h"

#include <algorithm>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <memory>
#include <mutex>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "assert.h"
#include "elf.h"
// #include "log.h"

#define warn(...) printf(__VA_ARGS__)
#define panic(...) printf(__VA_ARGS__)

using std::endl;
using std::get;
using std::ifstream;
using std::ignore;
using std::make_pair;
using std::tie;
using std::unique_ptr;

static bool xedInitDone = false;
static std::mutex initMutex;

// A non-reader
TraceReader::TraceReader() : trace_ready_(false), skipped_(0), buf_size_(0) {
}

// Trace Reader
TraceReader::TraceReader(const std::string& _trace, uint32_t _buf_size)
    : trace_ready_(false), warn_not_found_(1), skipped_(0), buf_size_(_buf_size) {
}

TraceReader::~TraceReader() {
  if (skipped_ > 0) {
    warn("Skipped %lu stray memory references\n", skipped_);
  }
}

bool TraceReader::operator!() {
  // Return true if there was an initialization error
  return !(trace_ready_);
}

void TraceReader::init(const std::string& _trace) {
  // Initialize XED only once
  initMutex.lock();
  if (!xedInitDone) {
    xed_tables_init();
    xedInitDone = true;
  }
  initMutex.unlock();

  // Set the XED machine mode to 64-bit
  xed_state_init2(&xed_state_, XED_MACHINE_MODE_LONG_64, XED_ADDRESS_WIDTH_64b);

  // Clear the 'invalid' record (memset() would do too)
  invalid_info_.pc = 0;
  invalid_info_.ins = nullptr;
  invalid_info_.pid = 0;
  invalid_info_.tid = 0;
  invalid_info_.target = 0;
  invalid_info_.mem_addr[0] = 0;
  invalid_info_.mem_addr[1] = 0;
  invalid_info_.mem_used[0] = false;
  invalid_info_.mem_used[1] = false;
  invalid_info_.custom_op = CustomOp::NONE;
  invalid_info_.taken = false;
  invalid_info_.unknown_type = false;
  invalid_info_.valid = false;

  if (_trace.size())
    traceFileIs(_trace);
  init_buffer();
}

void TraceReader::traceFileIs(const std::string& _trace) {
  trace_ = _trace;
  trace_ready_ = initTrace();
}

void TraceReader::fillCache(uint64_t _vAddr, uint8_t _reported_size, uint8_t* inst_bytes) {
  uint64_t size;
  uint8_t* loc;
  if (inst_bytes != NULL || locationForVAddr(_vAddr, &loc, &size)) {
    xed_map_.emplace(_vAddr, make_tuple(0, false, false, false, make_unique<xed_decoded_inst_t>()));
    xed_decoded_inst_t* ins = get<MAP_XED>(xed_map_.at(_vAddr)).get();
    xed_decoded_inst_zero_set_mode(ins, &xed_state_);
    if (inst_bytes != NULL) {
      loc = inst_bytes;
    }
    xed_error_enum_t res;
    res = xed_decode(ins, loc, _reported_size);
    if (res != XED_ERROR_NONE) {
      warn("XED decode error for 0x%lx: %s %u, replacing with nop\n", _vAddr, xed_error_enum_t2str(res),
           _reported_size);
      *ins = *makeNop(_reported_size);
    }
    // Record if this instruction requires memory operands, since the trace
    // will deliver it in additional pieces
    uint32_t n_mem_ops = xed_decoded_inst_number_of_memory_operands(ins);
    if (n_mem_ops > 0) {
      // NOPs are special and don't actually cause memory accesses
      xed_category_enum_t category = xed_decoded_inst_get_category(ins);
      if (category != XED_CATEGORY_NOP && category != XED_CATEGORY_WIDENOP) {
        uint32_t n_used_mem_ops = 0;  // 'lea' doesn't actually touch memory
        for (uint32_t i = 0; i < n_mem_ops; i++) {
          if (xed_decoded_inst_mem_read(ins, i)) {
            n_used_mem_ops++;
          }
          if (xed_decoded_inst_mem_written(ins, i)) {
            n_used_mem_ops++;
          }
        }
        if (n_used_mem_ops > 0) {
          if (n_used_mem_ops > 2) {
            warn("Unexpected %u memory operands for 0x%lx\n", n_used_mem_ops, _vAddr);
          }
          get<MAP_MEMOPS>(xed_map_.at(_vAddr)) = n_used_mem_ops;
        }
      }
    }
    auto& xed_tuple = xed_map_[_vAddr];
    // Record if this instruction is a conditional branch
    bool is_cond_br = (xed_decoded_inst_get_category(ins) == XED_CATEGORY_COND_BR);
    get<MAP_COND>(xed_tuple) = is_cond_br;

    // Record if this instruction is a 'rep' type, which may indicate a
    // variable number of memory records for input formats like memtrace
    bool is_rep = xed_decoded_inst_get_attribute(ins, XED_ATTRIBUTE_REP) > 0;
    get<MAP_REP>(xed_tuple) = is_rep;
  } else {
    if (warn_not_found_ > 0) {
      warn_not_found_ -= 1;
      if (warn_not_found_ > 0) {
        warn("No information for instruction at address 0x%lx", _vAddr);
      } else {
        warn(
            "No information for instruction at address 0x%lx. "
            "Suppressing further messages",
            _vAddr);
      }
    }
    // Replace the unknown instruction with a NOP
    // NOTE: Unknown memory records are skipped, so 'rep' needs no special
    // handling here
    xed_map_.emplace(_vAddr, make_tuple(0, true, false, false, makeNop(_reported_size)));
  }
}

unique_ptr<xed_decoded_inst_t> TraceReader::makeNop(uint8_t _length) {
  // A 10-to-15-byte NOP instruction (direct XED support is only up to 9)
  static const char* nop15 = "\x66\x66\x66\x66\x66\x66\x2e\x0f\x1f\x84\x00\x00\x00\x00\x00";

  auto ptr = make_unique<xed_decoded_inst_t>();
  xed_decoded_inst_t* ins = ptr.get();
  xed_decoded_inst_zero_set_mode(ins, &xed_state_);
  xed_error_enum_t res;

  // The reported instruction length must be 1-15 bytes
  _length &= 0xf;
  assert(_length > 0);
  if (_length > 9) {
    int offset = 15 - _length;
    const uint8_t* pos = reinterpret_cast<const uint8_t*>(nop15 + offset);
    res = xed_decode(ins, pos, 15 - offset);
  } else {
    uint8_t buf[10];
    res = xed_encode_nop(&buf[0], _length);
    if (res != XED_ERROR_NONE) {
      warn("XED NOP encode error: %s", xed_error_enum_t2str(res));
    }
    res = xed_decode(ins, buf, sizeof(buf));
  }
  if (res != XED_ERROR_NONE) {
    warn("XED NOP decode error: %s", xed_error_enum_t2str(res));
  }
  return ptr;
}

// WARNING: This function generates a memory leak!
xed_decoded_inst_t* TraceReader::createJmp(uint64_t displacement) {
  static int createdJmps = 0;
  xed_encoder_instruction_t inst;
  xed_state_t state;
  state.mmode = XED_MACHINE_MODE_LONG_64;
  xed_encoder_request_t req;
  xed_inst1(&inst, state, XED_ICLASS_JMP, 64,
            xed_relbr(displacement - 5, 32));  // -5 is due to this jump insn being 5 bytes large (1 op, 4 32 bit disp)
  xed_encoder_request_zero_set_mode(&req, &state);
  if (!xed_convert_to_encoder_request(&req, &inst)) {
    panic("Encoder conversion failed! Is the displacement too large?");
    return nullptr;
  }
  xed_uint8_t encodedBytes[15];
  unsigned int numBytesUsed = 0;
  xed_error_enum_t error = xed_encode(&req, encodedBytes, sizeof(encodedBytes), &numBytesUsed);
  if (error != XED_ERROR_NONE) {
    panic("Failed to encode due to: %s\n", xed_error_enum_t2str(error));
    return nullptr;
  }
  xed_decoded_inst_t* decoded_inst = new xed_decoded_inst_t;
  createdJmps++;
  if ((createdJmps % 1000) == 0)
    warn("generated %i Jmp instructions, possible memory leak", createdJmps);
  xed_decoded_inst_zero(decoded_inst);
  xed_decoded_inst_set_mode(decoded_inst, XED_MACHINE_MODE_LONG_64, XED_ADDRESS_WIDTH_64b);
  error = xed_decode(decoded_inst, encodedBytes, numBytesUsed);
  if (error == XED_ERROR_NONE) {
    return decoded_inst;
  }
  delete decoded_inst;
  panic("Could not decode due to %s\n", xed_error_enum_t2str(error));
  return nullptr;
}

void TraceReader::init_buffer() {
  // Push one dummy entry so we can pop in nextInstruction()
  ins_buffer.emplace_back(InstInfo());
  for (uint32_t i = 0; i < buf_size_; i++) {
    ins_buffer.emplace_back(*getNextInstruction());
  }
}

const InstInfo* TraceReader::nextInstruction() {
  ins_buffer.pop_front();
  ins_buffer.emplace_back(*getNextInstruction());
  return &ins_buffer.front();
}

// Find the next buffer entry, starting from ref, that matches the given PC
const TraceReader::returnValue TraceReader::findPC(bufferEntry& ref, uint64_t _pc) {
  for (; ref != ins_buffer.end(); ref++) {
    if (ref->pc == _pc) {
      return ENTRY_VALID;
    }
  }
  return ENTRY_NOT_FOUND;
}

const TraceReader::returnValue TraceReader::peekInstructionAtIndex(uint32_t idx, bufferEntry& ref) {
  if (idx >= ins_buffer.size())
    return ENTRY_NOT_FOUND;

  ref = ins_buffer.begin();
  for (; ref != ins_buffer.end(); ref++) {
    if (idx == 0)
      break;
    idx--;
  }
  return ENTRY_VALID;
}

const TraceReader::returnValue TraceReader::findPCInSegment(bufferEntry& ref, uint64_t _pc, uint64_t _termination_pc) {
  if (ref == ins_buffer.end())
    return ENTRY_NOT_FOUND;

  for (ref++; ref != ins_buffer.end(); ref++) {
    if (ref->pc == _pc) {
      return ENTRY_VALID;
    } else if (ref->pc == _termination_pc) {
      return ENTRY_OUT_OF_SEGMENT;
    }
  }
  return ENTRY_NOT_FOUND;
}

TraceReader::bufferEntry TraceReader::bufferStart() {
  return ins_buffer.begin();
}
