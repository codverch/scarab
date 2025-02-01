#include "unfused_committed_hist_ld.h"

UnfusedCommittedHistLd:: UnfusedCommittedHistLd(): nextCommitNum(0) {
    uchLdEntries.resize(MAX_UCH_LD_ENTRIES, {false, 0, 0});

}

void UnfusedCommittedHistLd::createEntry(uint64_t cachelineAddr) {
    // Extract the tag by masking out the lower 6 bits (assuming cacheline of size 64-bytes)
    uint32_t tag = cachelineAddr & ~0x3F;

    // Create a new entry
    UnfusedCommittedHistLdEntry entry;
    entry.valid_bit = true;
    entry.tag_bits = tag;
    entry.commit_num = nextCommitNum++;

    // Add the new entry
    uchLdEntries.push_back(entry);

    // Ensure there are only MAX_UCH_LD_ENTRIES entries
    if (uchLdEntries.size() > MAX_UCH_LD_ENTRIES) {
        // Find the entry with the smallest commit number (LRU entry)
        auto lruEntry = std::min_element(uchLdEntries.begin(), uchLdEntries.end(),
            [](const UnfusedCommittedHistLdEntry& a, const UnfusedCommittedHistLdEntry& b) {
                return a.commit_num < b.commit_num;
            });

        // Remove the LRU entry
        uchLdEntries.erase(lruEntry);
    }
}

void UnfusedCommittedHistLd::invalidateEntry(uint64_t cachelineAddr) {

    // Extract the 32-bit tag
    uint32_t tag = (cachelineAddr >> 6) & 0xFFFFFFFF;

    // Find and invalidate the matching entry
    for (auto& entry : uchLdEntries) {
        if (entry.valid_bit && entry.tag_bits == tag) {
            entry.valid_bit = false; // Invalidate the entry
            break;
        }
    }
}

uint8_t computeDistance(uint8_t curr_comm_num, uint8_t entry_comm_num) {

    return ((curr_comm_num - entry_comm_num) & 0x7F); //  Ensure 7-bit distance

}

bool UnfusedCommittedHistLd::findMatch(uint64_t cachelineAddr) {

    // Extract the 32-bit tag
    uint32_t tag = (cachelineAddr >> 6) & 0xFFFFFFFF;

    // Get the current commit number and increment it
    uint8_t currCommitNum = nextCommitNum;
    nextCommitNum = (nextCommitNum + 1) & 0x7F; // Ensure 7-bit commit number

    // Search for a matching entry
    for (auto& entry : uchLdEntries) {
        if (entry.valid_bit && entry.tag_bits == tag) {
            // Calculate the distance between the current μ-op and the matching entry
            uint8_t distance = computeDistance(currCommitNum, entry.commit_num);

            // Invalidate the matching entry (μ-ops can only fuse with one other μ-op)
            invalidateEntry(cachelineAddr);

            return true; // Match found
        }
    }

    return false; // No match found
}