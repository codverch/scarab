#ifndef UNFUSED_COMMITTED_HIST_ST_H_
#define UNFUSED_COMMITTED_HIST_ST_H_

#include <cstdint>
#include <vector>


struct UnfusedCommittedHistStEntry {
    bool valid_bit;       // Indicates whether the entry is valid
    uint32_t tag_bits;    // Tag bits derived from the cacheline address
};

// Unfused Committed History for Stores
class UnfusedCommittedHistSt {
private:
    std::vector<UnfusedCommittedHistStEntry> uchSTEntries;  // Vector to store history entries

public:
    // Constructor
    UnfusedCommittedHistSt();

    // Creates a new entry in the history
    void createEntry(uint64_t cachelineAddr);

    // Invalidates an entry in the history
    void invalidateEntry(uint64_t cachelineAddr);

    // Finds a match for a given cacheline address and invalidates it if found
    bool findMatch(uint64_t cachelineAddr);
};

#endif  // UNFUSED_COMMITTED_HIST_ST_H_