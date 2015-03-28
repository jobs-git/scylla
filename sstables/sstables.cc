/*
 * Copyright 2015 Cloudius Systems
 */

#include "log.hh"
#include <vector>
#include <typeinfo>
#include <limits>
#include "core/future.hh"
#include "core/future-util.hh"
#include "core/sstring.hh"
#include "core/fstream.hh"
#include "core/shared_ptr.hh"
#include <boost/algorithm/string.hpp>
#include <iterator>

#include "types.hh"
#include "sstables.hh"
#include "compress.hh"

namespace sstables {

class random_access_reader {
    input_stream<char> _in;
protected:
    virtual input_stream<char> open_at(uint64_t pos) = 0;
public:
    future<temporary_buffer<char>> read_exactly(size_t n) {
        return _in.read_exactly(n);
    }
    void seek(uint64_t pos) {
        _in = open_at(pos);
    }
    bool eof() { return _in.eof(); }
    virtual ~random_access_reader() { }
};

class file_random_access_reader : public random_access_reader {
    lw_shared_ptr<file> _file;
    size_t _buffer_size;
public:
    virtual input_stream<char> open_at(uint64_t pos) override {
        return make_file_input_stream(_file, pos, _buffer_size);
    }
    explicit file_random_access_reader(file&& f, size_t buffer_size = 8192)
        : file_random_access_reader(make_lw_shared<file>(std::move(f)), buffer_size) {}

    explicit file_random_access_reader(lw_shared_ptr<file> f, size_t buffer_size = 8192)
        : _file(f), _buffer_size(buffer_size)
    {
        seek(0);
    }
};

// FIXME: We don't use this API, and it can be removed. The compressed reader
// is only needed for the data file, and for that we have the nicer
// data_stream_at() API below.
class compressed_file_random_access_reader : public random_access_reader {
    lw_shared_ptr<file> _file;
    sstables::compression* _cm;
public:
    explicit compressed_file_random_access_reader(
                lw_shared_ptr<file> f, sstables::compression* cm)
        : _file(std::move(f))
        , _cm(cm)
    {
        seek(0);
    }
    compressed_file_random_access_reader(compressed_file_random_access_reader&&) = default;
    virtual input_stream<char> open_at(uint64_t pos) override {
        return make_compressed_file_input_stream(_file, _cm, pos);
    }

};

thread_local logging::logger sstlog("sstable");

std::unordered_map<sstable::version_types, sstring, enum_hash<sstable::version_types>> sstable::_version_string = {
    { sstable::version_types::la , "la" }
};

std::unordered_map<sstable::format_types, sstring, enum_hash<sstable::format_types>> sstable::_format_string = {
    { sstable::format_types::big , "big" }
};

std::unordered_map<sstable::component_type, sstring, enum_hash<sstable::component_type>> sstable::_component_map = {
    { component_type::Index, "Index.db"},
    { component_type::CompressionInfo, "CompressionInfo.db" },
    { component_type::Data, "Data.db" },
    { component_type::TOC, "TOC.txt" },
    { component_type::Summary, "Summary.db" },
    { component_type::Digest, "Digest.sha1" },
    { component_type::CRC, "CRC.db" },
    { component_type::Filter, "Filter.db" },
    { component_type::Statistics, "Statistics.db" },
};

// This assumes that the mappings are small enough, and called unfrequent
// enough.  If that changes, it would be adviseable to create a full static
// reverse mapping, even if it is done at runtime.
template <typename Map>
static typename Map::key_type reverse_map(const typename Map::mapped_type& value, Map& map) {
    for (auto& pair: map) {
        if (pair.second == value) {
            return pair.first;
        }
    }
    throw std::out_of_range("unable to reverse map");
}

struct bufsize_mismatch_exception : malformed_sstable_exception {
    bufsize_mismatch_exception(size_t size, size_t expected) :
        malformed_sstable_exception(sprint("Buffer improperly sized to hold requested data. Got: %ld. Expected: %ld", size, expected))
    {}
};

// This should be used every time we use read_exactly directly.
//
// read_exactly is a lot more convenient of an interface to use, because we'll
// be parsing known quantities.
//
// However, anything other than the size we have asked for, is certainly a bug,
// and we need to do something about it.
static void check_buf_size(temporary_buffer<char>& buf, size_t expected) {
    if (buf.size() < expected) {
        throw bufsize_mismatch_exception(buf.size(), expected);
    }
}

template <typename T, typename U>
static void check_truncate_and_assign(T& to, const U from) {
    static_assert(std::is_integral<T>::value && std::is_integral<U>::value, "T and U must be integral");
    if (from >= std::numeric_limits<T>::max()) {
        throw std::overflow_error("assigning U to T would cause an overflow");
    }
    to = from;
}

// Base parser, parses an integer type
template <typename T>
typename std::enable_if_t<std::is_integral<T>::value, void>
read_integer(temporary_buffer<char>& buf, T& i) {
    auto *nr = reinterpret_cast<const net::packed<T> *>(buf.get());
    i = net::ntoh(*nr);
}

template <typename T>
typename std::enable_if_t<std::is_integral<T>::value, future<>>
parse(random_access_reader& in, T& i) {
    return in.read_exactly(sizeof(T)).then([&i] (auto buf) {
        check_buf_size(buf, sizeof(T));

        read_integer(buf, i);
        return make_ready_future<>();
    });
}

template <typename T>
typename std::enable_if_t<std::is_integral<T>::value, future<>>
write(output_stream<char>& out, T i) {
    auto *nr = reinterpret_cast<const net::packed<T> *>(&i);
    i = net::hton(*nr);
    auto p = reinterpret_cast<const char*>(&i);
    return out.write(p, sizeof(T)).then([&out] (...) -> future<> {
        // TODO: handle result
        return make_ready_future<>();
    });
}

template <typename T>
typename std::enable_if_t<std::is_enum<T>::value, future<>>
parse(random_access_reader& in, T& i) {
    return parse(in, reinterpret_cast<typename std::underlying_type<T>::type&>(i));
}

template <typename T>
typename std::enable_if_t<std::is_enum<T>::value, future<>>
write(output_stream<char>& out, T i) {
    return write(out, reinterpret_cast<typename std::underlying_type<T>::type>(i));
}

future<> parse(random_access_reader& in, bool& i) {
    return parse(in, reinterpret_cast<uint8_t&>(i));
}

future<> write(output_stream<char>& out, bool i) {
    return write(out, static_cast<uint8_t>(i));
}

template <typename To, typename From>
static inline To convert(From f) {
    static_assert(sizeof(To) == sizeof(From), "Sizes must match");
    union {
        To to;
        From from;
    } conv;

    conv.from = f;
    return conv.to;
}

future<> parse(random_access_reader& in, double& d) {
    return in.read_exactly(sizeof(double)).then([&d] (auto buf) {
        check_buf_size(buf, sizeof(double));

        auto *nr = reinterpret_cast<const net::packed<unsigned long> *>(buf.get());
        d = convert<double>(net::ntoh(*nr));
        return make_ready_future<>();
    });
}

future<> write(output_stream<char>& out, double d) {
    auto *nr = reinterpret_cast<const net::packed<unsigned long> *>(&d);
    auto tmp = net::hton(*nr);
    auto p = reinterpret_cast<const char*>(&tmp);
    return out.write(p, sizeof(unsigned long)).then([&out] (...) -> future<> {
        // TODO: handle result
        return make_ready_future<>();
    });
}

template <typename T>
future<> parse(random_access_reader& in, T& len, sstring& s) {
    return in.read_exactly(len).then([&s, len] (auto buf) {
        check_buf_size(buf, len);
        s = sstring(buf.get(), len);
    });
}

future<> write(output_stream<char>& out, sstring& s) {
    return out.write(s).then([&out, &s] (...) -> future<> {
        // TODO: handle result
        return make_ready_future<>();
    });
}

// All composite parsers must come after this
template<typename First, typename... Rest>
future<> parse(random_access_reader& in, First& first, Rest&&... rest) {
    return parse(in, first).then([&in, &rest...] {
        return parse(in, std::forward<Rest>(rest)...);
    });
}

template<typename First, typename... Rest>
future<> write(output_stream<char>& out, First& first, Rest&&... rest) {
    return write(out, first).then([&out, &rest...] {
        return write(out, rest...);
    });
}

// Intended to be used for a type that describes itself through describe_type().
template <class T>
typename std::enable_if_t<!std::is_integral<T>::value && !std::is_enum<T>::value, future<>>
parse(random_access_reader& in, T& t) {
    return t.describe_type([&in] (auto&&... what) -> future<> {
        return parse(in, what...);
    });
}

template <class T>
typename std::enable_if_t<!std::is_integral<T>::value && !std::is_enum<T>::value, future<>>
write(output_stream<char>& out, T& t) {
    return t.describe_type([&out] (auto&&... what) -> future<> {
        return write(out, what...);
    });
}

// For all types that take a size, we provide a template that takes the type
// alone, and another, separate one, that takes a size parameter as well, of
// type Size. This is because although most of the time the size and the data
// are contiguous, it is not always the case. So we want to have the
// flexibility of parsing them separately.
template <typename Size>
future<> parse(random_access_reader& in, disk_string<Size>& s) {
    auto len = std::make_unique<Size>();
    auto f = parse(in, *len);
    return f.then([&in, &s, len = std::move(len)] {
        return parse(in, *len, s.value);
    });
}

template <typename Size>
future<> write(output_stream<char>& out, disk_string<Size>& s) {
    Size len = 0;
    check_truncate_and_assign(len, s.value.size());
    return write(out, len).then([&out, &s] {
        return write(out, s.value);
    });
}

// We cannot simply read the whole array at once, because we don't know its
// full size. We know the number of elements, but if we are talking about
// disk_strings, for instance, we have no idea how much of the stream each
// element will take.
//
// Sometimes we do know the size, like the case of integers. There, all we have
// to do is to convert each member because they are all stored big endian.
// We'll offer a specialization for that case below.
template <typename Size, typename Members>
typename std::enable_if_t<!std::is_integral<Members>::value, future<>>
parse(random_access_reader& in, Size& len, std::vector<Members>& arr) {

    auto count = make_lw_shared<size_t>(0);
    auto eoarr = [count, len] { return *count == len; };

    return do_until(eoarr, [count, &in, &arr] {
        return parse(in, arr[(*count)++]);
    });
}

template <typename Size, typename Members>
typename std::enable_if_t<std::is_integral<Members>::value, future<>>
parse(random_access_reader& in, Size& len, std::vector<Members>& arr) {
    return in.read_exactly(len * sizeof(Members)).then([&arr, len] (auto buf) {
        check_buf_size(buf, len * sizeof(Members));

        auto *nr = reinterpret_cast<const net::packed<Members> *>(buf.get());
        for (size_t i = 0; i < len; ++i) {
            arr[i] = net::ntoh(nr[i]);
        }
        return make_ready_future<>();
    });
}

// We resize the array here, before we pass it to the integer / non-integer
// specializations
template <typename Size, typename Members>
future<> parse(random_access_reader& in, disk_array<Size, Members>& arr) {
    auto len = std::make_unique<Size>();
    auto f = parse(in, *len);
    return f.then([&in, &arr, len = std::move(len)] {
        arr.elements.resize(*len);
        return parse(in, *len, arr.elements);
    });
}


template <typename Members>
typename std::enable_if_t<!std::is_integral<Members>::value, future<>>
write(output_stream<char>& out, std::vector<Members>& arr) {

    auto count = make_lw_shared<size_t>(0);
    auto eoarr = [count, &arr] { return *count == arr.size(); };

    return do_until(eoarr, [count, &out, &arr] {
        return write(out, arr[(*count)++]);
    });
}

template <typename Members>
typename std::enable_if_t<std::is_integral<Members>::value, future<>>
write(output_stream<char>& out, std::vector<Members>& arr) {
    std::vector<Members> tmp;
    tmp.resize(arr.size());
    // copy arr into tmp converting each entry into big-endian representation.
    auto *nr = reinterpret_cast<const net::packed<Members> *>(arr.data());
    for (size_t i = 0; i < arr.size(); i++) {
        tmp[i] = net::hton(nr[i]);
    }
    auto p = reinterpret_cast<const char*>(tmp.data());
    auto bytes = tmp.size() * sizeof(Members);
    return out.write(p, bytes).then([&out] (...) -> future<> {
        return make_ready_future<>();
    });
}

template <typename Size, typename Members>
future<> write(output_stream<char>& out, disk_array<Size, Members>& arr) {
    Size len = 0;
    check_truncate_and_assign(len, arr.elements.size());
    return write(out, len).then([&out, &arr] {
        return write(out, arr.elements);
    });
}

template <typename Size, typename Key, typename Value>
future<> parse(random_access_reader& in, Size& len, std::unordered_map<Key, Value>& map) {
    auto count = make_lw_shared<Size>();
    auto eos = [len, count] { return len == *count; };
    return do_until(eos, [len, count, &in, &map] {
        struct kv {
            Key key;
            Value value;
        };
        ++*count;

        auto el = std::make_unique<kv>();
        auto f = parse(in, el->key, el->value);
        return f.then([el = std::move(el), &map] {
            map.emplace(el->key, el->value);
        });
    });
}

template <typename Size, typename Key, typename Value>
future<> parse(random_access_reader& in, disk_hash<Size, Key, Value>& h) {
    auto w = std::make_unique<Size>();
    auto f = parse(in, *w);
    return f.then([&in, &h, w = std::move(w)] {
        return parse(in, *w, h.map);
    });
}

future<> parse(random_access_reader& in, summary& s) {
    using pos_type = typename decltype(summary::positions)::value_type;

    return parse(in, s.header.min_index_interval,
                     s.header.size,
                     s.header.memory_size,
                     s.header.sampling_level,
                     s.header.size_at_full_sampling).then([&in, &s] {
        return in.read_exactly(s.header.size * sizeof(pos_type)).then([&in, &s] (auto buf) {
            auto len = s.header.size * sizeof(pos_type);
            check_buf_size(buf, len);

            s.entries.resize(s.header.size);

            auto *nr = reinterpret_cast<const pos_type *>(buf.get());
            s.positions = std::vector<pos_type>(nr, nr + s.header.size);

            // Since the keys in the index are not sized, we need to calculate
            // the start position of the index i+1 to determine the boundaries
            // of index i. The "memory_size" field in the header determines the
            // total memory used by the map, so if we push it to the vector, we
            // can guarantee that no conditionals are used, and we can always
            // query the position of the "next" index.
            s.positions.push_back(s.header.memory_size);
        }).then([&in, &s] {
            in.seek(sizeof(summary::header) + s.header.memory_size);
            return parse(in, s.first_key, s.last_key);
        }).then([&in, &s] {

            in.seek(s.positions[0] + sizeof(summary::header));

            assert(s.positions.size() == (s.entries.size() + 1));

            auto idx = make_lw_shared<size_t>(0);
            return do_for_each(s.entries.begin(), s.entries.end(), [idx, &in, &s] (auto& entry) {
                auto pos = s.positions[(*idx)++];
                auto next = s.positions[*idx];

                auto entrysize = next - pos;

                return in.read_exactly(entrysize).then([&entry, entrysize] (auto buf) {
                    check_buf_size(buf, entrysize);

                    auto keysize = entrysize - 8;
                    entry.key = bytes(buf.get(), keysize);
                    buf.trim_front(keysize);
                    read_integer(buf, entry.position);

                    return make_ready_future<>();
                });
            }).then([&s] {
                // Since we've made the decision of reading the whole entries array upfront, we can
                // actually get rid of the positions. We won't need it anymore.
                std::vector<typename decltype(s.positions)::value_type>().swap(s.positions);
            });
        });
    });
}

future<summary_entry&> sstable::read_summary_entry(size_t i) {
    // The last one is the boundary marker
    if (i >= (_summary.entries.size())) {
        throw std::out_of_range(sprint("Invalid Summary index: %ld", i));
    }

    return make_ready_future<summary_entry&>(_summary.entries[i]);
}

future<> parse(random_access_reader& in, struct replay_position& rp) {
    return parse(in, rp.segment, rp.position);
}

future<> parse(random_access_reader& in, estimated_histogram::eh_elem &e) {
    return parse(in, e.offset, e.bucket);
}

future<> parse(random_access_reader& in, estimated_histogram &e) {
    return parse(in, e.elements);
}

future<> parse(random_access_reader& in, streaming_histogram &h) {
    return parse(in, h.max_bin_size, h.hash);
}

future<> parse(random_access_reader& in, validation_metadata& m) {
    return parse(in, m.partitioner, m.filter_chance);
}

future<> parse(random_access_reader& in, compaction_metadata& m) {
    return parse(in, m.ancestors, m.cardinality);
}

future<> parse(random_access_reader& in, index_entry& ie) {
    return parse(in, ie.key, ie.position, ie.promoted_index);
}

future<> parse(random_access_reader& in, deletion_time& d) {
    return parse(in, d.local_deletion_time, d.marked_for_delete_at);
}

template <typename Child>
future<> parse(random_access_reader& in, std::unique_ptr<metadata>& p) {
    p.reset(new Child);
    return parse(in, *static_cast<Child *>(p.get()));
}

future<> parse(random_access_reader& in, stats_metadata& m) {
    return parse(in,
        m.estimated_row_size,
        m.estimated_column_count,
        m.position,
        m.min_timestamp,
        m.max_timestamp,
        m.max_local_deletion_time,
        m.compression_ratio,
        m.estimated_tombstone_drop_time,
        m.sstable_level,
        m.repaired_at,
        m.min_column_names,
        m.max_column_names,
        m.has_legacy_counter_shards
    );
}

future<> parse(random_access_reader& in, statistics& s) {
    return parse(in, s.hash).then([&in, &s] {
        return do_for_each(s.hash.map.begin(), s.hash.map.end(), [&in, &s] (auto val) mutable {
            in.seek(val.second);

            switch (val.first) {
                case metadata_type::Validation:
                    return parse<validation_metadata>(in, s.contents[val.first]);
                case metadata_type::Compaction:
                    return parse<compaction_metadata>(in, s.contents[val.first]);
                case metadata_type::Stats:
                    return parse<stats_metadata>(in, s.contents[val.first]);
                default:
                    sstlog.warn("Invalid metadata type at Statistics file: {} ", int(val.first));
                    return make_ready_future<>();
                }
        });
    });
}

// This is small enough, and well-defined. Easier to just read it all
// at once
future<> sstable::read_toc() {
    auto file_path = filename(sstable::component_type::TOC);

    sstlog.debug("Reading TOC file {} ", file_path);

    return engine().open_file_dma(file_path, open_flags::ro).then([this] (file f) {
        auto bufptr = allocate_aligned_buffer<char>(4096, 4096);
        auto buf = bufptr.get();

        auto fut = f.dma_read(0, buf, 4096);
        return std::move(fut).then([this, f = std::move(f), bufptr = std::move(bufptr)] (size_t size) {
            // This file is supposed to be very small. Theoretically we should check its size,
            // but if we so much as read a whole page from it, there is definitely something fishy
            // going on - and this simplifies the code.
            if (size >= 4096) {
                throw malformed_sstable_exception("SSTable too big: " + to_sstring(size) + " bytes.");
            }

            std::experimental::string_view buf(bufptr.get(), size);
            std::vector<sstring> comps;

            boost::split(comps , buf, boost::is_any_of("\n"));

            for (auto& c: comps) {
                // accept trailing newlines
                if (c == "") {
                    continue;
                }
                try {
                   _components.insert(reverse_map(c, _component_map));
                } catch (std::out_of_range& oor) {
                    throw malformed_sstable_exception("Unrecognized TOC component: " + c);
                }
            }
            if (!_components.size()) {
                throw malformed_sstable_exception("Empty TOC");
            }
            return make_ready_future<>();
        });
    }).then_wrapped([file_path] (future<> f) {
        try {
            f.get();
        } catch (std::system_error& e) {
            if (e.code() == std::error_code(ENOENT, std::system_category())) {
                throw malformed_sstable_exception(file_path + ": file not found");
            }
        }
    });

}

future<index_list> sstable::read_indexes(uint64_t position, uint64_t quantity) {
    struct reader {
        uint64_t count = 0;
        std::vector<index_entry> indexes;
        file_random_access_reader stream;
        reader(lw_shared_ptr<file> f, uint64_t quantity) : stream(f) { indexes.reserve(quantity); }
    };

    auto r = make_lw_shared<reader>(_index_file, quantity);

    r->stream.seek(position);

    auto end = [r, quantity] { return r->count >= quantity; };

    return do_until(end, [this, r] {
        r->indexes.emplace_back();
        auto fut = parse(r->stream, r->indexes.back());
        return std::move(fut).then_wrapped([this, r] (future<> f) mutable {
            try {
               f.get();
               r->count++;
            } catch (bufsize_mismatch_exception &e) {
                // We have optimistically emplaced back one element of the
                // vector. If we have failed to parse, we should remove it
                // so size() gives us the right picture.
                r->indexes.pop_back();

                // FIXME: If the file ends at an index boundary, there is
                // no problem. Essentially, we can't know how many indexes
                // are in a sampling group, so there isn't really any way
                // to know, other than reading.
                //
                // If, however, we end in the middle of an index, this is a
                // corrupted file. This code is not perfect because we only
                // know that an exception happened, and it happened due to
                // eof. We don't really know if eof happened at the index
                // boundary.  To know that, we would have to keep track of
                // the real position of the stream (including what's
                // already in the buffer) before we start to read the
                // index, and after. We won't go through such complexity at
                // the moment.
                if (r->stream.eof()) {
                    r->count = std::numeric_limits<std::remove_reference<decltype(r->count)>::type>::max();
                } else {
                    throw e;
                }
            }
            return make_ready_future<>();
        });
    }).then([r] {
        return make_ready_future<index_list>(std::move(r->indexes));
    });
}

template <typename T, sstable::component_type Type, T sstable::* Comptr>
future<> sstable::read_simple() {

    auto file_path = filename(Type);
    sstlog.debug(("Reading " + _component_map[Type] + " file {} ").c_str(), file_path);
    return engine().open_file_dma(file_path, open_flags::ro).then([this] (file f) {

        auto r = std::make_unique<file_random_access_reader>(std::move(f), 4096);
        auto fut = parse(*r, *this.*Comptr);
        return fut.then([r = std::move(r)] {});
    }).then_wrapped([this, file_path] (future<> f) {
        try {
            f.get();
        } catch (std::system_error& e) {
            if (e.code() == std::error_code(ENOENT, std::system_category())) {
                throw malformed_sstable_exception(file_path + ": file not found");
            }
        }
    });
}

template <typename T, sstable::component_type Type, T sstable::* Comptr>
future<> sstable::write_simple() {

    auto file_path = filename(Type);
    sstlog.debug(("Writing " + _component_map[Type] + " file {} ").c_str(), file_path);
    return engine().open_file_dma(file_path, open_flags::wo | open_flags::create | open_flags::truncate).then([this] (file f) {

        auto out = make_file_output_stream(make_lw_shared<file>(std::move(f)), 4096);
        auto w = make_shared<output_stream<char>>(std::move(out));
        auto fut = write(*w, *this.*Comptr);
        return fut.then([w] {
            return w->flush().then([w] {
                return w->close().then([w] {}); // the underlying file is synced here.
            });
        });
    }).then_wrapped([this, file_path] (future<> f) {
        try {
            f.get();
        } catch (std::system_error& e) {
            // TODO: handle exception.
        }
    });
}

future<> sstable::read_compression() {
     // FIXME: If there is no compression, we should expect a CRC file to be present.
    if (!has_component(sstable::component_type::CompressionInfo)) {
        return make_ready_future<>();
    }

    return read_simple<compression, component_type::CompressionInfo, &sstable::_compression>();
}

future<> sstable::write_compression() {
    if (!has_component(sstable::component_type::CompressionInfo)) {
        return make_ready_future<>();
    }

    return write_simple<compression, component_type::CompressionInfo, &sstable::_compression>();
}

future<> sstable::read_statistics() {
    return read_simple<statistics, component_type::Statistics, &sstable::_statistics>();
}

future<> sstable::open_data() {
    return when_all(engine().open_file_dma(filename(component_type::Index), open_flags::ro),
                    engine().open_file_dma(filename(component_type::Data), open_flags::ro)).then([this] (auto files) {
        _index_file = make_lw_shared<file>(std::move(std::get<file>(std::get<0>(files).get())));
        _data_file  = make_lw_shared<file>(std::move(std::get<file>(std::get<1>(files).get())));
        return _data_file->size().then([this] (auto size) {
          _data_file_size = size;
        });
    });
}

future<> sstable::load() {
    return read_toc().then([this] {
        return read_statistics();
    }).then([this] {
        return read_compression();
    }).then([this] {
        return read_filter();
    }).then([this] {;
        return read_summary();
    }).then([this] {
        return open_data();
    }).then([this] {
        // After we have _compression and _data_file_size, we can update
        // _compression with additional information it needs:
        if (has_component(sstable::component_type::CompressionInfo)) {
            _compression.update(_data_file_size);
        }
    });
}

future<> sstable::store() {
    // TODO: write other components as well.
    return write_compression().then([this] {
        return write_filter();
    });
}

const bool sstable::has_component(component_type f) {
    return _components.count(f);
}

const sstring sstable::filename(component_type f) {

    auto& version = _version_string.at(_version);
    auto& format = _format_string.at(_format);
    auto& component = _component_map.at(f);
    auto generation =  to_sstring(_generation);

    return _dir + "/" + version + "-" + generation + "-" + format + "-" + component;
}

sstable::version_types sstable::version_from_sstring(sstring &s) {
    return reverse_map(s, _version_string);
}

sstable::format_types sstable::format_from_sstring(sstring &s) {
    return reverse_map(s, _format_string);
}

input_stream<char> sstable::data_stream_at(uint64_t pos) {
    if (_compression) {
        return make_compressed_file_input_stream(
                _data_file, &_compression, pos);
    } else {
        return make_file_input_stream(_data_file, pos);
    }
}

// FIXME: to read a specific byte range, we shouldn't use the input stream
// interface - it may cause too much read when we intend to read a small
// range, and too small reads, and repeated waits, when reading a large range
// which we should have started at once.
future<temporary_buffer<char>> sstable::data_read(uint64_t pos, size_t len) {
    auto stream = std::make_unique<input_stream<char>>(data_stream_at(pos));
    auto fut = stream->read_exactly(len);
    return fut.then([stream = std::move(stream)] (temporary_buffer<char> buf) { return buf; });
}

}
