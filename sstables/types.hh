#pragma once

#include "core/enum.hh"
#include "bytes.hh"

namespace sstables {

// Some in-disk structures have an associated integer (of varying sizes) that
// represents how large they are. They can be a byte-length, in the case of a
// string, number of elements, in the case of an array, etc.
//
// For those elements, we encapsulate the underlying type in an outter
// structure that embeds how large is the in-disk size. It is a lot more
// convenient to embed it in the size than explicitly writing it in the parser.
// This way, we don't need to encode this information in multiple places at
// once - it is already part of the type.
template <typename Size>
struct disk_string {
    sstring value;
};

template <typename Size, typename Members>
struct disk_array {
    static_assert(std::is_integral<Size>::value, "Length type must be convertible to integer");
    std::vector<Members> elements;
};

template <typename Size, typename Key, typename Value>
struct disk_hash {
    std::unordered_map<Key, Value, std::hash<Key>> map;
};

struct option {
    disk_string<uint16_t> key;
    disk_string<uint16_t> value;

    template <typename Describer>
    future<> describe_type(Describer f) { return f(key, value); }
};

struct filter {
    uint32_t hashes;
    disk_array<uint32_t, uint64_t> buckets;

    template <typename Describer>
    future<> describe_type(Describer f) { return f(hashes, buckets); }
};

struct index_entry {
    disk_string<uint16_t> key;
    uint64_t position;
    disk_string<uint32_t> promoted_index;
};

struct summary_entry {
    bytes key;
    uint64_t position;
};

// Note: Sampling level is present in versions ka and higher. We ATM only support la,
// so it's always there. But we need to make this conditional if we ever want to support
// other formats (unlikely)
struct summary_la {
    struct header {
        // The minimum possible amount of indexes per group (sampling level)
        uint32_t min_index_interval;
        // The number of entries in the Summary File
        uint32_t size;
        // The memory to be consumed to map the whole Summary into memory.
        uint64_t memory_size;
        // The actual sampling level.
        uint32_t sampling_level;
        // The number of entries the Summary *would* have if the sampling
        // level would be equal to min_index_interval.
        uint32_t size_at_full_sampling;
    } header;
    // The position in the Summary file for each of the indexes.
    // NOTE1 that its actual size is determined by the "size" parameter, not
    // by its preceding size_at_full_sampling
    // NOTE2: They are laid out in *MEMORY* order, not BE.
    // NOTE3: The sizes in this array represent positions in the memory stream,
    // not the file. The memory stream effectively begins after the header,
    // so every position here has to be added of sizeof(header).
    std::vector<uint32_t> positions;
    std::vector<summary_entry> entries;

    disk_string<uint32_t> first_key;
    disk_string<uint32_t> last_key;
};
using summary = summary_la;

struct estimated_histogram {
    struct eh_elem {
        uint64_t offset;
        uint64_t bucket;
    };

    disk_array<uint32_t, eh_elem> elements;
};

struct replay_position {
    uint64_t segment;
    uint32_t position;
};

struct streaming_histogram {
    uint32_t max_bin_size;
    disk_hash<uint32_t, double, uint64_t> hash;
};

struct metadata {
};

struct validation_metadata : public metadata {
    disk_string<uint16_t> partitioner;
    double filter_chance;
};

struct compaction_metadata : public metadata {
    disk_array<uint32_t, uint32_t> ancestors;
    disk_array<uint32_t, uint8_t> cardinality;
};

struct la_stats_metadata : public metadata {
    estimated_histogram estimated_row_size;
    estimated_histogram estimated_column_count;
    replay_position position;
    uint64_t min_timestamp;
    uint64_t max_timestamp;
    uint32_t max_local_deletion_time;
    double compression_ratio;
    streaming_histogram estimated_tombstone_drop_time;
    uint32_t sstable_level;
    uint64_t repaired_at;
    disk_array<uint32_t, disk_string<uint16_t>> min_column_names;
    disk_array<uint32_t, disk_string<uint16_t>> max_column_names;
    bool has_legacy_counter_shards;
};
using stats_metadata = la_stats_metadata;

// Numbers are found on disk, so they do matter. Also, setting their sizes of
// that of an uint32_t is a bit wasteful, but it simplifies the code a lot
// since we can now still use a strongly typed enum without introducing a
// notion of "disk-size" vs "memory-size".
enum class metadata_type : uint32_t {
    Validation = 0,
    Compaction = 1,
    Stats = 2,
};

}

namespace std {

template <>
struct hash<sstables::metadata_type> : enum_hash<sstables::metadata_type> {};

}

namespace sstables {

struct statistics {
    disk_hash<uint32_t, metadata_type, uint32_t> hash;
    std::unordered_map<metadata_type, std::unique_ptr<metadata>> contents;
};

struct deletion_time {
    uint32_t local_deletion_time;
    uint64_t marked_for_delete_at;
};

enum class column_mask : uint8_t {
    none = 0x0,
    deletion = 0x01,
    expiration = 0x02,
    counter = 0x04,
    counter_update = 0x08,
    range_tombstone = 0x10,
};

inline column_mask operator&(column_mask m1, column_mask m2) {
    return column_mask(static_cast<uint8_t>(m1) & static_cast<uint8_t>(m2));
}

inline column_mask operator|(column_mask m1, column_mask m2) {
    return column_mask(static_cast<uint8_t>(m1) | static_cast<uint8_t>(m2));
}
}

