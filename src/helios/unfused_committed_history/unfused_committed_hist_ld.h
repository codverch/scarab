#ifndef UNFUSED_COMMITTED_HIST_LD_H_
#define UNFUSED_COMMITTED_HIST_LD_H_

#include <cstdint>
#include <vector>
#include <algorithm>

#define MAX_UCH_LD_ENTRIES 6 

// An entry in the Unfused Committed History for Loads (UCHL)
struct UnfusedCommittedHistLdEntry {
    bool valid_bit;      // 1-bit valid bit stored as a byte
    uint32_t tag_bits;   // 32-bit tag for tag match using partial cacheline address
    uint8_t commit_num;  // 7-bit commit number stored as a byte
};

// Unfused Committed History for Loads (UCHL)
class UnfusedCommittedHistLd {
private:
    std::vector<UnfusedCommittedHistLdEntry> uchLdEntries;  // 6-entry UCH for loads
    uint8_t nextCommitNum;  // To track the next commit number to assign

public:
    // Constructor
    UnfusedCommittedHistLd();

    // Create an entry in UCH for loads
    void createEntry(uint64_t cachelineAddr);

    // Invalidate an entry in UCH for loads
    void invalidateEntry(uint64_t cachelineAddr);

    uint8_t computeDistance(uint8_t curr_comm_num, uint8_t entry_comm_num); 

    // Search for a matching entry in UCH for loads
    bool findMatch(uint64_t cachelineAddr);
};

#endif  // UNFUSED_COMMITTED_HIST_LD_H_