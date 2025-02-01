#include "unfused_committed_hist_st.h"


UnfusedCommittedHistSt::UnfusedCommittedHistSt() {
    uchSTEntries.resize(1, {false, 0});  // Initialize with one invalid entry
}

// Creates a new entry in the UCH Store 
void UnfusedCommittedHistSt::createEntry(uint64_t cachelineAddr) {
    uint32_t tag = (cachelineAddr >> 6) & 0xFFFFFFFF;  // Extract tag from cacheline address
    uchSTEntries[0].valid_bit = true;                  // Mark the entry as valid
    uchSTEntries[0].tag_bits = tag;                    // Store the tag
}

// Invalidates an entry in the history if it matches the given cacheline address
void UnfusedCommittedHistSt::invalidateEntry(uint64_t cachelineAddr) {
    uint32_t tag = (cachelineAddr >> 6) & 0xFFFFFFFF;  // Extract tag from cacheline address
    if (uchSTEntries[0].valid_bit && uchSTEntries[0].tag_bits == tag) {
        uchSTEntries[0].valid_bit = false;  // Invalidate the entry
    }
}

// Finds a match for the given cacheline address and invalidates it if found
bool UnfusedCommittedHistSt::findMatch(uint64_t cachelineAddr) {
    uint32_t tag = (cachelineAddr >> 6) & 0xFFFFFFFF;  // Extract tag from cacheline address
    if (uchSTEntries[0].valid_bit && uchSTEntries[0].tag_bits == tag) {
        invalidateEntry(cachelineAddr);  // Invalidate the entry if it matches
        return true;                     // Return true indicating a match was found
    }
    return false;  // Return false if no match was found
}