/**
 *  @file usearch.hpp
 *  @author Ashot Vardanian
 *  @brief Single-header Vector Search.
 *  @date 2023-04-26
 *
 *  @copyright Copyright (c) 2023
 */
#pragma once
#include <algorithm> // `std::sort_heap`
#include <atomic>    // `std::atomic`
#include <bitset>    // `std::bitset`
#include <climits>   // `CHAR_BIT`
#include <cmath>     // `std::sqrt`
#include <cstring>   // `std::memset`
#include <mutex>     // `std::unique_lock` - replacement candidate
#include <random>    // `std::default_random_engine` - replacement candidate
#include <unistd.h>  // `open`, `close`
#include <utility>   // `std::exchange`
#include <vector>    // `std::vector`

#if defined(WIN32) || defined(_WIN32) || defined(__WIN32__) || defined(__NT__)
#else
#include <fcntl.h>    // `fallocate`
#include <stdlib.h>   // `posix_memalign`
#include <sys/mman.h> // `mmap`
#include <sys/stat.h> // `fstat` for file size
#endif

#if defined(__GNUC__)
// https://gcc.gnu.org/onlinedocs/gcc/Other-Builtins.html
// Zero means we are only going to read from that memory.
// Three means high temporal locality and suggests to keep
// the data in all layers of cache.
#define prefetch_m(ptr) __builtin_prefetch((void*)(ptr), 0, 3)
#elif defined(__x86_64__)
#define prefetch_m(ptr) _mm_prefetch((void*)(ptr), _MM_HINT_T0)
#else
#define prefetch_m(ptr)
#endif

#ifdef NDEBUG
#define assert_m(must_be_true, message)
#else
#define assert_m(must_be_true, message)                                                                                \
    if (!(must_be_true)) {                                                                                             \
        throw std::runtime_error(message);                                                                             \
    }
#endif

#define usearch_align_m __attribute__((aligned(64)))
#define usearch_pack_m __attribute__((packed))

namespace unum {
namespace usearch {

using f64_t = double;
using f32_t = float;

/**
 *  GCC supports half-precision types.
 *  https://gcc.gnu.org/onlinedocs/gcc/Half-Precision.html
 */
enum class f16_t : std::uint16_t {};

template <typename at> at angle_to_radians(at angle) noexcept { return angle * M_PI / 180.f; }

template <typename at> at square(at value) noexcept { return value * value; }

inline std::size_t ceil2(std::size_t v) noexcept {
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v |= v >> 32;
    v++;
    return v;
}

template <typename scalar_at> struct ip_gt {
    using scalar_t = scalar_at;
    using result_type = scalar_t;

    inline scalar_t operator()(scalar_t const* a, scalar_t const* b, std::size_t dim, std::size_t = 0) const noexcept {
        result_type ab{};
#if defined(__GNUC__)
#pragma GCC ivdep
#elif defined(__clang__)
#pragma clang loop vectorize(enable)
#elif defined(_OPENMP)
#pragma omp simd reduction(+ : ab)
#endif
        for (std::size_t i = 0; i != dim; ++i)
            ab += a[i] * b[i];
        return 1 - ab;
    }
};

template <typename scalar_at> struct cos_gt {
    using scalar_t = scalar_at;
    using result_type = scalar_t;

    inline scalar_t operator()(scalar_t const* a, scalar_t const* b, std::size_t dim, std::size_t = 0) const noexcept {
        result_type ab{}, a2{}, b2{};
#if defined(__GNUC__)
#pragma GCC ivdep
#elif defined(__clang__)
#pragma clang loop vectorize(enable)
#elif defined(_OPENMP)
#pragma omp simd reduction(+ : ab, a2, b2)
#endif
        for (std::size_t i = 0; i != dim; ++i)
            ab += a[i] * b[i], a2 += a[i] * a[i], b2 += b[i] * b[i];
        return ab / (std::sqrt(a2) * std::sqrt(b2));
    }
};

template <typename scalar_at> struct l2_squared_gt {
    using scalar_t = scalar_at;
    using result_type = scalar_t;

    inline scalar_t operator()(scalar_t const* a, scalar_t const* b, std::size_t dim, std::size_t = 0) const noexcept {
        result_type ab_deltas_sq{};
#if defined(__GNUC__)
#pragma GCC ivdep
#elif defined(__clang__)
#pragma clang loop vectorize(enable)
#elif defined(_OPENMP)
#pragma omp simd reduction(+ : ab_deltas_sq)
#endif
        for (std::size_t i = 0; i != dim; ++i)
            ab_deltas_sq += (a[i] - b[i]) * (a[i] - b[i]);
        return ab_deltas_sq;
    }
};

/**
 *  @brief  Hamming distance computes the number of differing bits in
 *          two arrays of integers. An example would be a textual document,
 *          tokenized and hashed into a fixed-capacity bitset.
 */
template <typename scalar_at> struct bit_hamming_gt {
    using scalar_t = scalar_at;
    using result_type = std::size_t;
    static_assert(std::is_unsigned<scalar_t>::value, "Hamming distance requires unsigned integral words");
    static constexpr std::size_t bits_per_word_k = sizeof(scalar_t) * CHAR_BIT;

    inline std::size_t operator()(scalar_t const* a, scalar_t const* b, std::size_t words,
                                  std::size_t = 0) const noexcept {
        std::size_t matches{};
#if defined(__GNUC__)
#pragma GCC ivdep
#elif defined(__clang__)
#pragma clang loop vectorize(enable)
#elif defined(_OPENMP)
#pragma omp simd reduction(+ : matches)
#endif
        for (std::size_t i = 0; i != words; ++i)
            matches += std::bitset<bits_per_word_k>(a[i] ^ b[i]).count();
        return matches;
    }
};

/**
 *  @brief  Counts the number of matching elements in two unique sorted sets.
 *          Can be used to compute the similarity between two textual documents
 *          using the IDs of tokens present in them.
 */
template <typename scalar_at> struct jaccard_gt {
    using scalar_t = scalar_at;
    using result_type = f32_t;
    static_assert(!std::is_floating_point<scalar_t>::value, "Jaccard distance requires integral scalars");

    inline result_type operator()( //
        scalar_t const* a, scalar_t const* b, std::size_t a_length, std::size_t b_length) const noexcept {
        std::size_t intersection{};
        std::size_t i{};
        std::size_t j{};
        while (i != a_length && j != b_length) {
            intersection += a[i] == b[j];
            i += a[i] < b[j];
            j += a[i] >= b[j];
        }
        return 1.f - intersection * 1.f / (a_length + b_length - intersection);
    }
};

/**
 *  @brief  Counts the number of matching elements in two unique sorted sets.
 *          Can be used to compute the similarity between two textual documents
 *          using the IDs of tokens present in them.
 */
template <typename scalar_at> struct pearson_correlation_gt {
    using scalar_t = scalar_at;
    using result_type = scalar_t;

    inline result_type operator()( //
        scalar_t const* a, scalar_t const* b, std::size_t dim, std::size_t = 0) const noexcept {
        scalar_t a_sum{}, b_sum{}, ab_sum{};
        scalar_t a_sq_sum{}, b_sq_sum{};
#if defined(__GNUC__)
#pragma GCC ivdep
#elif defined(__clang__)
#pragma clang loop vectorize(enable)
#elif defined(_OPENMP)
#pragma omp simd reduction(+ : a_sum, b_sum, ab_sum, a_sq_sum, b_sq_sum)
#endif
        for (std::size_t i = 0; i != dim; ++i) {
            a_sum += a[i];
            b_sum += b[i];
            ab_sum += a[i] * b[i];
            a_sq_sum += a[i] * a[i];
            b_sq_sum += b[i] * b[i];
        }
        result_type denom = std::sqrt((dim * a_sq_sum - a_sum * a_sum) * (dim * b_sq_sum - b_sum * b_sum));
        result_type corr = (dim * ab_sum - a_sum * b_sum) / denom;
        return -corr;
    }
};

/**
 *  @brief  Haversine distance for the shortest distance between two nodes on
 *          the surface of a 3D sphere, defined with latitude and longitude.
 */
template <typename scalar_at> struct haversine_gt {
    using scalar_t = scalar_at;
    using result_type = scalar_t;
    static_assert(!std::is_integral<scalar_t>::value, "Latitude and longitude must be floating-node");

    inline scalar_t operator()(scalar_t const* a, scalar_t const* b, std::size_t = 2, std::size_t = 2) const noexcept {
        scalar_t lat_a = a[0], lon_a = a[1];
        scalar_t lat_b = b[0], lon_b = b[1];

        scalar_t lat_delta = angle_to_radians<scalar_t>(lat_b - lat_a);
        scalar_t lon_delta = angle_to_radians<scalar_t>(lon_b - lon_a);

        scalar_t converted_lat_a = angle_to_radians<scalar_t>(lat_a);
        scalar_t converted_lat_b = angle_to_radians<scalar_t>(lat_b);

        scalar_t x = square(std::sin(lat_delta / 2.f)) +
                     std::cos(converted_lat_a) * std::cos(converted_lat_b) * square(std::sin(lon_delta / 2.f));

        return std::atan2(std::sqrt(x), std::sqrt(1.f - x));
    }
};

template <std::size_t multiple_ak> inline std::size_t divide_round_up(std::size_t num) noexcept {
    return (num + multiple_ak - 1) / multiple_ak;
}

template <typename allocator_at = std::allocator<char>> class visits_bitset_gt {
    using allocator_t = allocator_at;
    using byte_t = typename allocator_t::value_type;
    static_assert(sizeof(byte_t) == 1, "Allocator must allocate separate addressable bytes");

    using slot_t = std::uint64_t;

    std::uint64_t* u64s_{};
    std::size_t slots_{};

  public:
    visits_bitset_gt() noexcept {}
    visits_bitset_gt(std::size_t capacity) noexcept {
        slots_ = divide_round_up<64>(capacity);
        u64s_ = (slot_t*)allocator_t{}.allocate(slots_ * sizeof(slot_t));
    }
    ~visits_bitset_gt() noexcept { allocator_t{}.deallocate((byte_t*)u64s_, slots_ * sizeof(slot_t)), u64s_ = nullptr; }
    void clear() noexcept { std::memset(u64s_, 0, slots_ * sizeof(slot_t)); }
    inline bool test(std::size_t i) const noexcept { return u64s_[i / 64] & (1ul << (i & 63ul)); }
    inline void set(std::size_t i) noexcept { u64s_[i / 64] |= (1ul << (i & 63ul)); }

    visits_bitset_gt(visits_bitset_gt&& other) noexcept {
        u64s_ = other.u64s_;
        slots_ = other.slots_;
        other.u64s_ = nullptr;
        other.slots_ = 0;
    }

    visits_bitset_gt& operator=(visits_bitset_gt&& other) noexcept {
        std::swap(u64s_, other.u64s_);
        std::swap(slots_, other.slots_);
        return *this;
    }
};

using visits_bitset_t = visits_bitset_gt<>;

/**
 *  @brief  Similar to `std::priority_queue`, but allows raw access to underlying
 *          memory, in case you want to shuffle it or sort.
 */
template <typename element_at,                                //
          typename comparator_at = std::less<void>,           // <void> is needed before C++14.
          typename allocator_at = std::allocator<element_at>> //
class max_heap_gt {
  public:
    using element_t = element_at;
    using comparator_t = comparator_at;
    using allocator_t = allocator_at;

    using value_type = element_t;

  private:
    element_t* elements_;
    std::size_t size_;
    std::size_t capacity_;
    std::size_t max_capacity_;

  public:
    max_heap_gt(std::size_t max_capacity = 0) noexcept
        : elements_(nullptr), size_(0), capacity_(0), max_capacity_(max_capacity) {}

    bool empty() const noexcept { return !size_; }
    std::size_t size() const noexcept { return size_; }
    std::size_t capacity() const noexcept { return capacity_; }
    element_t const& top() const noexcept { return elements_[0]; }
    element_t const& max() const noexcept { return elements_[0]; }

    void clear() noexcept {
        while (size_) {
            size_--;
            elements_[size_].~element_t();
        }
    }

    bool reserve(std::size_t n) noexcept {
        if (n < capacity_)
            return true;
        if (max_capacity_ && capacity_ == max_capacity_)
            return false;

        auto new_capacity = std::max<std::size_t>(capacity_ * 2u, 16u);
        if (max_capacity_)
            new_capacity = std::min(new_capacity, max_capacity_);

        auto allocator = allocator_t{};
        auto new_elements = allocator.allocate(new_capacity);
        if (!new_elements)
            return false;

        if (elements_) {
            std::uninitialized_copy_n(elements_, size_, new_elements);
            allocator.deallocate(elements_, capacity_);
        }
        elements_ = new_elements;
        capacity_ = new_capacity;
        return new_elements;
    }

    template <typename... args_at> //
    bool emplace(args_at&&... args) noexcept {
        if (!reserve(size_ + 1))
            return false;

        new (&elements_[size_]) element_t({std::forward<args_at>(args)...});
        size_++;
        shift_up(size_ - 1);
        return true;
    }

    element_t pop() noexcept {
        element_t result = top();
        std::swap(elements_[0], elements_[size_ - 1]);
        size_--;
        elements_[size_].~element_t();
        shift_down(0);
        return result;
    }

    /** @brief Invalidates the "max-heap" property, transforming into ascending range. */
    void sort_ascending() noexcept { std::sort_heap(elements_, elements_ + size_, &less); }
    /** @brief Invalidates the "max-heap" property, transforming into descending range. */
    void sort_descending() noexcept { sort_ascending(), std::reverse(elements_, elements_ + size_); }
    void sort_heap() noexcept { std::make_heap(elements_, elements_ + size_, &less); }
    void shrink(std::size_t n) noexcept { size_ = n; }

    element_t* data() noexcept { return elements_; }
    element_t const* data() const noexcept { return elements_; }

  private:
    inline std::size_t parent_idx(std::size_t i) const noexcept { return (i - 1u) / 2u; }
    inline std::size_t left_child_idx(std::size_t i) const noexcept { return (i * 2u) + 1u; }
    inline std::size_t right_child_idx(std::size_t i) const noexcept { return (i * 2u) + 2u; }
    static bool less(element_t const& a, element_t const& b) noexcept { return comparator_t{}(a, b); }

    void shift_up(std::size_t i) noexcept {
        for (; i && less(elements_[parent_idx(i)], elements_[i]); i = parent_idx(i))
            std::swap(elements_[parent_idx(i)], elements_[i]);
    }

    void shift_down(std::size_t i) noexcept {
        std::size_t max_idx = i;

        std::size_t left = left_child_idx(i);
        if (left < size_ && less(elements_[max_idx], elements_[left]))
            max_idx = left;

        std::size_t right = right_child_idx(i);
        if (right < size_ && less(elements_[max_idx], elements_[right]))
            max_idx = right;

        if (i != max_idx) {
            std::swap(elements_[i], elements_[max_idx]);
            shift_down(max_idx);
        }
    }
};

/**
 *
 */
class mutex_t {
    using slot_t = std::int32_t;
    slot_t flag_;

  public:
    inline mutex_t(slot_t flag = 0) noexcept : flag_(flag) {}
    inline ~mutex_t() noexcept {}

    inline bool try_lock() noexcept {
        slot_t raw = 0;
        return __atomic_compare_exchange_n(&flag_, &raw, 1, true, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
    }

    inline void lock() noexcept {
        slot_t raw;
    lock_again:
        raw = 0;
        if (!__atomic_compare_exchange_n(&flag_, &raw, 1, true, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            goto lock_again;
    }

    inline void unlock() noexcept { __atomic_store_n(&flag_, 0, __ATOMIC_RELEASE); }
};

static_assert(sizeof(mutex_t) == sizeof(std::int32_t), "Mutex is larger than expected");

using lock_t = std::unique_lock<mutex_t>;

/**
 *  @brief Five-byte integer type to address node clouds with over 4B entries.
 */
class usearch_pack_m uint40_t {
    unsigned char octets[5];

  public:
    inline uint40_t() noexcept { std::memset(octets, 0, 5); }
    inline uint40_t(std::uint32_t n) noexcept { std::memcpy(octets + 1, (char*)&n, 4), octets[0] = 0; }
    inline uint40_t(std::uint64_t n) noexcept { std::memcpy(octets, (char*)&n + 3, 5); }
#if defined(__clang__)
    inline uint40_t(std::size_t n) noexcept { std::memcpy(octets, (char*)&n + 3, 5); }
#endif

    uint40_t(uint40_t&&) = default;
    uint40_t(uint40_t const&) = default;
    uint40_t& operator=(uint40_t&&) = default;
    uint40_t& operator=(uint40_t const&) = default;

    inline uint40_t& operator+=(std::uint32_t i) noexcept {
        std::uint32_t& tail = *reinterpret_cast<std::uint32_t*>(octets + 1);
        octets[0] += static_cast<unsigned char>((tail + i) < tail);
        tail += i;
        return *this;
    }

    inline operator std::size_t() const noexcept {
        std::size_t result = 0;
        std::memcpy((char*)&result + 3, octets, 5);
        return result;
    }

    inline uint40_t& operator++() noexcept { return *this += 1; }

    inline uint40_t operator++(int) noexcept {
        uint40_t old = *this;
        *this += 1;
        return old;
    }
};

static_assert(sizeof(uint40_t) == 5, "uint40_t must be exactly 5 bytes");

template <typename scalar_at> class span_gt {
    scalar_at* data_;
    std::size_t size_;

  public:
    span_gt(scalar_at* begin, scalar_at* end) noexcept : data_(begin), size_(end - begin) {}
    span_gt(scalar_at* begin, std::size_t size) noexcept : data_(begin), size_(size) {}
    scalar_at* data() const noexcept { return data_; }
    std::size_t size() const noexcept { return size_; }
    operator scalar_at*() const noexcept { return data(); }
};

/**
 *  @brief
 *      Configuration settings for the index construction.
 *      Includes the main `::connectivity` parameter (`M` in the paper)
 *      and two expansion factors - for construction and search.
 */
struct config_t {

    /// @brief Number of neighbors per graph node.
    /// Defaults to 32 in FAISS and 16 in hnswlib.
    /// > It is called `M` in the paper.
    static constexpr std::size_t connectivity_default_k = 16;

    /// @brief Hyper-parameter controlling the quality of indexing.
    /// Defaults to 40 in FAISS and 200 in hnswlib.
    /// > It is called `efConstruction` in the paper.
    static constexpr std::size_t expansion_add_default_k = 128;

    /// @brief Hyper-parameter controlling the quality of search.
    /// Defaults to 16 in FAISS and 10 in hnswlib.
    /// > It is called `ef` in the paper.
    static constexpr std::size_t expansion_search_default_k = 64;

    std::size_t connectivity = connectivity_default_k;
    std::size_t expansion_add = expansion_add_default_k;
    std::size_t expansion_search = expansion_search_default_k;

    ///
    std::size_t max_elements = 0;
    std::size_t max_threads_add = 64;
    std::size_t max_threads_search = 64;
};

/**
 *  @brief
 *      Approximate Nearest Neighbors Search index using the
 *      Hierarchical Navigable Small World graph algorithm.
 *
 *  @section Features
 *      - Search for vectors of different dimensionality.
 *      - Thread-safe.
 *      - Bring your threads!
 *
 *  @tparam metric_at
 *      A function vector responsible for computing the distance between two vectors.
 *      Must overload the call operator with the following signature:
 *          - `result_type (*) (scalar_t const *, scalar_t const *, std::size_t, std::size_t)`
 *
 *  @tparam label_at
 *      The type of unique labels to assign to vectors.
 *
 *  @tparam id_at
 *      The smallest unsigned integer type to address indexed elements.
 *      Can be a built-in `uint32_t`, `uint64_t`, or our custom `uint40_t`.
 *
 *  @tparam allocator_at
 *      Dynamic memory allocator.
 */
template <typename metric_at = ip_gt<float>,            //
          typename label_at = std::size_t,              //
          typename id_at = std::uint32_t,               //
          typename scalar_at = float,                   //
          typename allocator_at = std::allocator<char>> //
class index_gt {
  public:
    using metric_t = metric_at;
    using scalar_t = scalar_at;
    using label_t = label_at;
    using id_t = id_at;
    using allocator_t = allocator_at;

    using distance_t =
        typename std::result_of<metric_t(scalar_t const*, scalar_t const*, std::size_t, std::size_t)>::type;

  private:
    using neighbors_count_t = id_t;
    using dim_t = std::uint32_t;
    using level_t = std::int32_t;

    using allocator_traits_t = std::allocator_traits<allocator_t>;
    using byte_t = typename allocator_t::value_type;
    static_assert(sizeof(byte_t) == 1, "Allocator must allocate separate addressable bytes");
    static constexpr std::size_t base_level_multiple_k = 2;

    using visits_bitset_t = visits_bitset_gt<allocator_t>;

    struct precomputed_constants_t {
        double inverse_log_connectivity{};
        std::size_t connectivity_max_base{};
        std::size_t neighbors_bytes{};
        std::size_t neighbors_base_bytes{};
        std::size_t mutex_bytes{};
    };
    struct distance_and_id_t {
        distance_t first;
        id_t second;
    };
    struct compare_by_distance_t {
        inline bool operator()(distance_and_id_t a, distance_and_id_t b) const noexcept { return a.first < b.first; }
    };

    using distances_and_ids_allocator_t = typename allocator_traits_t::template rebind_alloc<distance_and_id_t>;
    using distances_and_ids_t = max_heap_gt<distance_and_id_t, compare_by_distance_t, distances_and_ids_allocator_t>;

    struct neighbors_ref_t {
        neighbors_count_t& count;
        id_t* neighbors{};

        inline neighbors_ref_t(byte_t* tape) noexcept
            : count(*(neighbors_count_t*)tape), neighbors((neighbors_count_t*)tape + 1) {}
    };

    struct usearch_pack_m node_head_t {
        label_t label;
        dim_t dim;
        level_t level;
        // Variable length array, that has multiple similarly structured segments.
        // Each starts with a `neighbors_count_t` and is followed by such number of `id_t`s.
        byte_t neighbors[1];
    };
    static constexpr std::size_t head_bytes_k = sizeof(label_t) + sizeof(dim_t) + sizeof(level_t);

    struct node_t {
        byte_t* tape_;
        scalar_t* vector_;
    };

    class node_ref_t {
        mutex_t* mutex_{};

      public:
        node_head_t& head{};
        scalar_t* vector{};

        inline node_ref_t(mutex_t& m, node_head_t& h, scalar_t* s) noexcept : mutex_(&m), head(h), vector(s) {}
        inline lock_t lock() const noexcept { return mutex_ ? lock_t{*mutex_} : lock_t{}; }
        inline operator node_t() const noexcept { return {mutex_ ? (byte_t*)mutex_ : (byte_t*)&head, vector}; }
    };

    struct usearch_align_m thread_context_t {
        distances_and_ids_t top_candidates;
        distances_and_ids_t candidates_set;
        distances_and_ids_t candidates_filter;
        visits_bitset_t visits;
        std::default_random_engine level_generator;
        metric_t metric;
    };

    config_t config_{};
    precomputed_constants_t pre_;
    int viewed_file_descriptor_{};

    usearch_align_m mutable std::atomic<std::size_t> capacity_{};
    usearch_align_m mutable std::atomic<std::size_t> size_{};

    mutex_t global_mutex_{};
    level_t max_level_{};
    id_t entry_id_{};

    using node_allocator_t = typename allocator_traits_t::template rebind_alloc<node_t>;
    std::vector<node_t, node_allocator_t> nodes_{};

    using thread_context_allocator_t = typename allocator_traits_t::template rebind_alloc<thread_context_t>;
    mutable std::vector<thread_context_t, thread_context_allocator_t> thread_contexts_;

  public:
    std::size_t connectivity() const noexcept { return config_.connectivity; }
    std::size_t capacity() const noexcept { return capacity_; }
    std::size_t size() const noexcept { return size_; }
    std::size_t max_threads_add() const noexcept { return config_.max_threads_add; }
    bool is_immutable() const noexcept { return viewed_file_descriptor_ != 0; }
    bool synchronize() const noexcept { return config_.max_threads_add > 1; }

    index_gt(config_t config = {}, metric_t metric = {}, allocator_t = {}) : config_(config) {

        // Externally defined hyper-parameters:
        config_.expansion_add = std::max(config_.expansion_add, config_.connectivity);
        pre_ = precompute(config);

        // Configure initial empty state:
        size_ = 0u;
        max_level_ = -1;
        entry_id_ = 0u;
        viewed_file_descriptor_ = 0;

        // Dynamic memory:
        thread_contexts_.resize(std::max(config.max_threads_search, config.max_threads_add));
        for (thread_context_t& context : thread_contexts_)
            context.metric = metric;
        reserve(config.max_elements);
    }

    ~index_gt() noexcept { clear(); }

#pragma region Adjusting Configuration

    void clear() noexcept {
        std::size_t n = size_;
        for (std::size_t i = 0; i != n; ++i)
            node_free(i);
        size_ = 0;
        max_level_ = -1;
        entry_id_ = 0u;
    }

    void reserve(std::size_t new_capacity) noexcept(false) {

        assert_m(new_capacity >= size_, "Can't drop existing values");
        nodes_.resize(new_capacity);
        for (thread_context_t& context : thread_contexts_)
            context.visits = visits_bitset_t(new_capacity);

        capacity_ = new_capacity;
    }

    static config_t optimize(config_t const& config) noexcept {
        precomputed_constants_t pre = precompute(config);
        std::size_t bytes_per_node_base = head_bytes_k + pre.neighbors_base_bytes + pre.mutex_bytes;
        std::size_t rounded_size = divide_round_up<64>(bytes_per_node_base) * 64;
        std::size_t added_connections = (rounded_size - rounded_size) / sizeof(id_t);
        config_t result = config;
        result.connectivity = config.connectivity + added_connections / base_level_multiple_k;
        return result;
    }

#pragma endregion

#pragma region Construction and Search

    id_t add(                                                //
        label_t new_label, span_gt<scalar_t const> new_span, //
        std::size_t thread_idx = 0, bool store_vector = true) {

        assert_m(!is_immutable(), "Can't add to an immutable index");
        id_t new_id = static_cast<id_t>(size_.fetch_add(1));
        scalar_t const* new_vector = new_span.data();
        std::size_t new_dim = new_span.size();

        // Determining how much memory to allocate depends on the target level.
        lock_t new_level_lock(global_mutex_);
        level_t max_level = max_level_;
        thread_context_t& context = thread_contexts_[thread_idx];
        level_t new_target_level = choose_random_level(context.level_generator);
        if (new_target_level <= max_level)
            new_level_lock.unlock();

        // Allocate the neighbors
        node_ref_t new_node = node_malloc(new_label, new_vector, new_dim, new_target_level, store_vector);
        lock_t new_lock = new_node.lock();
        nodes_[new_id] = new_node;

        // Do nothing for the first element
        if (!new_id) {
            max_level_ = new_target_level;
            return new_id;
        }

        // Go down the level, tracking only the closest match
        id_t closest_id = entry_id_;
        distance_t closest_dist =
            context.metric(new_vector, node(closest_id).vector, new_dim, node(closest_id).head.dim);
        for (level_t level = max_level; level > new_target_level; level--) {
            bool changed;
            do {
                changed = false;
                node_ref_t closest_node = node(closest_id);
                lock_t closest_lock = closest_node.lock();
                neighbors_ref_t closest_header = neighbors_non_base(closest_node, level);
                iterate_through_neighbors(closest_header, [&](id_t candidate_id) noexcept {
                    node_ref_t candidate_node = node(candidate_id);
                    distance_t candidate_dist =
                        context.metric(new_vector, candidate_node.vector, new_dim, candidate_node.head.dim);
                    if (candidate_dist < closest_dist) {
                        closest_dist = candidate_dist;
                        closest_id = candidate_id;
                        changed = true;
                    }
                });
            } while (changed);
        }

        // From `new_target_level` down perform proper extensive search.
        for (level_t level = std::min(new_target_level, max_level); level >= 0; level--) {
            search_to_insert(closest_id, new_vector, new_dim, level, context);
            closest_id = connect_new_element(new_id, level, context);
        }

        // Releasing lock for the maximum level
        if (new_target_level > max_level) {
            entry_id_ = new_id;
            max_level_ = new_target_level;
        }
        return new_id;
    }

    template <typename label_and_distance_callback_at>
    void search(                                                //
        span_gt<scalar_t const> query_span, std::size_t wanted, //
        label_and_distance_callback_at&& callback, std::size_t thread_idx = 0) const {

        if (!size_)
            return;

        scalar_t const* query_vec = query_span.data();
        std::size_t query_dim = query_span.size();

        // Go down the level, tracking only the closest match
        thread_context_t& context = thread_contexts_[thread_idx];
        id_t closest_id = entry_id_;
        distance_t closest_dist =
            context.metric(query_vec, node(closest_id).vector, query_dim, node(closest_id).head.dim);
        for (level_t level = max_level_; level > 0; level--) {
            bool changed;
            do {
                changed = false;
                node_ref_t closest_node = node(closest_id);
                neighbors_ref_t closest_header = neighbors_non_base(closest_node, level);
                iterate_through_neighbors(closest_header, [&](id_t candidate_id) noexcept {
                    node_ref_t candidate_node = node(candidate_id);
                    distance_t candidate_dist = context.metric( //
                        query_vec, candidate_node.vector, query_dim, candidate_node.head.dim);
                    if (candidate_dist < closest_dist) {
                        closest_dist = candidate_dist;
                        closest_id = candidate_id;
                        changed = true;
                    }
                });

            } while (changed);
        }

        // For bottom layer we need a more optimized procedure
        search_to_find_in_base( //
            closest_id, query_vec, query_dim, std::max(config_.expansion_search, wanted), context);
        while (context.top_candidates.size() > wanted)
            context.top_candidates.pop();

        while (context.top_candidates.size()) {
            distance_and_id_t top = context.top_candidates.top();
            callback(node(top.second).head.label, top.first);
            context.top_candidates.pop();
        }
    }

    std::size_t search(                                         //
        span_gt<scalar_t const> query_span, std::size_t wanted, //
        label_t* matches, distance_t* distances,                //
        std::size_t thread_idx = 0) const {

        std::size_t found = 0;
        auto callback = [&](label_t label, distance_t distance) noexcept {
            if (matches)
                matches[found] = label;
            if (distances)
                distances[found] = distance;
            ++found;
        };
        search(query_span, wanted, callback, thread_idx);
        std::reverse(matches, matches + found * (matches != nullptr));
        std::reverse(distances, distances + found * (distances != nullptr));
        return found;
    }

#pragma endregion

#pragma region Serialization

    struct state_t {
        // Check compatibility
        std::uint64_t bytes_per_label{};
        std::uint64_t bytes_per_id{};

        // Describe state
        std::uint64_t connectivity{};
        std::uint64_t size{};
        std::uint64_t entry_id{};
        std::uint64_t max_level{};
    };

    /**
     *  Compatibility: Linux, MacOS, Windows.
     */
    void save(char const* file_path) const noexcept(false) {

        state_t state;
        state.connectivity = config_.connectivity;
        state.size = size_;
        state.entry_id = entry_id_;
        state.max_level = max_level_;

        std::FILE* file = std::fopen(file_path, "w");
        if (!file)
            throw std::runtime_error(std::strerror(errno));

        // Write the header
        {
            std::size_t written = std::fwrite(&state, sizeof(state), 1, file);
            if (!written) {
                std::fclose(file);
                throw std::runtime_error(std::strerror(errno));
            }
        }

        // Serialize nodes one by one
        for (std::size_t i = 0; i != state.size; ++i) {
            node_ref_t node_ref = node(static_cast<id_t>(i));
            std::size_t bytes_to_dump = node_dump_size(node_ref.head.dim, node_ref.head.level);
            std::size_t written = std::fwrite(&node_ref.head, bytes_to_dump - node_ref.head.dim, 1, file);
            if (!written) {
                std::fclose(file);
                throw std::runtime_error(std::strerror(errno));
            }
            written = std::fwrite(node_ref.vector, node_ref.head.dim, 1, file);
            if (!written) {
                std::fclose(file);
                throw std::runtime_error(std::strerror(errno));
            }
        }

        std::fclose(file);
    }

    /**
     *  Compatibility: Linux, MacOS, Windows.
     */
    void load(char const* file_path) noexcept(false) {
        state_t state;
        std::FILE* file = std::fopen(file_path, "r");
        if (!file)
            throw std::runtime_error(std::strerror(errno));

        // Read the header
        {
            std::size_t read = std::fread(&state, sizeof(state), 1, file);
            if (!read) {
                std::fclose(file);
                throw std::runtime_error(std::strerror(errno));
            }
            config_.connectivity = state.connectivity;
            config_.max_elements = state.size;
            pre_ = precompute(config_);
            reserve(state.size);
            size_ = state.size;
            max_level_ = state.max_level;
            entry_id_ = state.entry_id;
        }

        // Load nodes one by one
        for (std::size_t i = 0; i != state.size; ++i) {
            node_head_t head;
            std::size_t read = std::fread(&head, head_bytes_k, 1, file);
            if (!read) {
                std::fclose(file);
                throw std::runtime_error(std::strerror(errno));
            }

            std::size_t bytes_to_dump = node_dump_size(head.dim, head.level);
            node_ref_t node_ref = node_malloc(head.label, nullptr, head.dim, head.level, true);
            read = std::fread((byte_t*)&node_ref.head + head_bytes_k, bytes_to_dump - head_bytes_k, 1, file);
            if (!read) {
                std::fclose(file);
                throw std::runtime_error(std::strerror(errno));
            }

            nodes_[i] = node_ref;
        }

        std::fclose(file);
        viewed_file_descriptor_ = 0;
    }

    /**
     *  Compatibility: Linux, MacOS.
     */
    void view(char const* file_path) noexcept(false) {
#if defined(WIN32) || defined(_WIN32) || defined(__WIN32__) || defined(__NT__)
        throw std::logic_error("Memory-mapping is not yet available for Windows");
#else
        state_t state;
        int open_flags = O_RDONLY;
#if __linux__
        open_flags |= O_NOATIME;
#endif
        int descriptor = open(file_path, open_flags);

        // Estimate the file size
        struct stat file_stat;
        int fstat_status = fstat(descriptor, &file_stat);
        if (fstat_status < 0) {
            close(descriptor);
            throw std::runtime_error(std::strerror(errno));
        }

        // Map the entire file
        byte_t* file = (byte_t*)mmap(NULL, file_stat.st_size, PROT_READ, MAP_PRIVATE, descriptor, 0);
        if (file == MAP_FAILED) {
            close(descriptor);
            throw std::runtime_error(std::strerror(errno));
        }

        // Read the header
        {
            std::memcpy(&state, file, sizeof(state));
            config_.connectivity = state.connectivity;
            config_.max_elements = state.size;
            config_.max_threads_add = 0;
            pre_ = precompute(config_);
            reserve(state.size);
            size_ = state.size;
            max_level_ = state.max_level;
            entry_id_ = state.entry_id;
        }

        // Locate every node packed into file
        std::size_t progress = sizeof(state);
        for (std::size_t i = 0; i != state.size; ++i) {
            node_head_t const& head = *(node_head_t const*)(file + progress);
            std::size_t bytes_to_dump = node_dump_size(head.dim, head.level);
            nodes_[i].tape_ = (byte_t*)(file + progress);
            nodes_[i].vector_ = (scalar_t*)(file + progress + bytes_to_dump - head.dim);
            progress += bytes_to_dump;
            max_level_ = std::max(max_level_, head.level);
        }

        viewed_file_descriptor_ = descriptor;
#endif
    }

#pragma endregion

  private:
    inline static precomputed_constants_t precompute(config_t const& config) noexcept {
        precomputed_constants_t pre;
        pre.connectivity_max_base = config.connectivity * base_level_multiple_k;
        pre.inverse_log_connectivity = 1.0 / std::log(static_cast<double>(config.connectivity));
        pre.neighbors_bytes = config.connectivity * sizeof(id_t) + sizeof(neighbors_count_t);
        pre.neighbors_base_bytes = pre.connectivity_max_base * sizeof(id_t) + sizeof(neighbors_count_t);
        pre.mutex_bytes = sizeof(mutex_t) * (config.max_threads_add > 1);
        return pre;
    }

    inline std::size_t node_dump_size(dim_t dim, level_t level) const noexcept {
        return head_bytes_k + pre_.neighbors_base_bytes + pre_.neighbors_bytes * level + sizeof(scalar_t) * dim;
    }

    void node_free(std::size_t id) noexcept {

        if (viewed_file_descriptor_)
            return;

        // This function is rarely called and can be as expensive as needed for higher space-efficiency.
        node_t& node = nodes_[id];
        if (!node.tape_)
            return;

        node_head_t const& head = *(node_head_t const*)(node.tape_ + pre_.mutex_bytes);
        std::size_t levels_bytes = pre_.neighbors_base_bytes + pre_.neighbors_bytes * head.level;
        bool store_vector = (byte_t*)(node.tape_ + pre_.mutex_bytes + head_bytes_k + levels_bytes) == //
                            (byte_t*)(node.vector_);
        std::size_t node_bytes =          //
            pre_.mutex_bytes +            // Optional concurrency-control
            head_bytes_k + levels_bytes + // Obligatory neighborhood index
            head.dim * store_vector       // Optional vector copy
            ;

        allocator_t{}.deallocate(node.tape_, node_bytes);
        node = {};
    }

    node_ref_t node_malloc(                                     //
        label_t label, scalar_t const* vector, std::size_t dim, //
        level_t level, bool store_vector = true) noexcept(false) {

        // This function is rarely called and can be as expensive as needed for higher space-efficiency.
        std::size_t levels_bytes = pre_.neighbors_base_bytes + pre_.neighbors_bytes * level;
        std::size_t node_bytes =                  //
            pre_.mutex_bytes +                    // Optional concurrency-control
            head_bytes_k + levels_bytes +         // Obligatory neighborhood index
            sizeof(scalar_t) * dim * store_vector // Optional vector copy
            ;

        byte_t* data = (byte_t*)allocator_t{}.allocate(node_bytes);
        assert_m(data, "Not enough memory for links");

        mutex_t* mutex = synchronize() ? (mutex_t*)data : nullptr;
        scalar_t* scalars = store_vector //
                                ? (scalar_t*)(data + pre_.mutex_bytes + head_bytes_k + levels_bytes)
                                : (scalar_t*)(vector);

        std::memset(data, 0, node_bytes);
        std::memcpy(scalars, vector, sizeof(scalar_t) * dim * (store_vector && vector));

        node_head_t& head = *(node_head_t*)(data + pre_.mutex_bytes);
        head.label = label;
        head.dim = static_cast<dim_t>(dim);
        head.level = level;

        return {*mutex, head, scalars};
    }

    inline node_ref_t node(id_t id) const noexcept {

        node_t node = nodes_[id];
        byte_t* data = node.tape_;
        mutex_t* mutex = synchronize() ? (mutex_t*)data : nullptr;
        node_head_t& head = *(node_head_t*)(data + pre_.mutex_bytes);
        scalar_t* scalars = node.vector_;

        return {*mutex, head, scalars};
    }

    inline neighbors_ref_t neighbors_base(node_ref_t node) const noexcept { return {node.head.neighbors}; }

    inline neighbors_ref_t neighbors_non_base(node_ref_t node, level_t level) const noexcept {
        return {node.head.neighbors + pre_.neighbors_base_bytes + (level - 1) * pre_.neighbors_bytes};
    }

    inline neighbors_ref_t neighbors(node_ref_t node, level_t level) const noexcept {
        return level ? neighbors_non_base(node, level) : neighbors_base(node);
    }

    id_t connect_new_element(id_t new_id, level_t level, thread_context_t& context) noexcept(false) {

        node_ref_t new_node = node(new_id);
        distances_and_ids_t& top_candidates = context.top_candidates;
        std::size_t connectivity_max = level ? config_.connectivity : pre_.connectivity_max_base;
        filter_top_candidates_with_heuristic(top_candidates, context.candidates_set, config_.connectivity, context);

        distance_and_id_t const* const top_unordered = top_candidates.data();
        std::size_t const top_count = top_candidates.size();
        id_t next_closest_entry_id = top_unordered[0].second;
        distance_t next_closest_distance = top_unordered[0].first;

        // Outgoing links from `new_id`:
        {
            neighbors_ref_t new_neighbors = neighbors(new_node, level);
            assert_m(!new_neighbors.count, "The newly inserted element should have blank link list");

            new_neighbors.count = static_cast<neighbors_count_t>(top_count);
            for (std::size_t idx = 0; idx < top_count; idx++) {
                assert_m(!new_neighbors.neighbors[idx], "Possible memory corruption");
                assert_m(level <= node(top_unordered[idx].second).head.level, "Linking to missing level");

                new_neighbors.neighbors[idx] = top_unordered[idx].second;
                if (top_unordered[idx].first < next_closest_distance) {
                    next_closest_entry_id = top_unordered[idx].second;
                    next_closest_distance = top_unordered[idx].first;
                }
            }
        }

        // Reverse links from the neighbors:
        for (std::size_t idx = 0; idx < top_count; idx++) {
            id_t close_id = top_unordered[idx].second;
            node_ref_t close_node = node(close_id);
            lock_t close_lock = close_node.lock();

            neighbors_ref_t close_header = neighbors(close_node, level);
            assert_m(close_header.count <= connectivity_max, "Possible corruption");
            assert_m(close_id != new_id, "Self-loops are impossible");
            assert_m(level <= close_node.head.level, "Linking to missing level");

            // If `new_id` is already present in the neighboring connections of `close_id`
            // then no need to modify any connections or run the heuristics.
            if (close_header.count < connectivity_max) {
                close_header.neighbors[close_header.count] = new_id;
                close_header.count++;
                continue;
            }

            // To fit a new connection we need to drop an existing one.
            distances_and_ids_t& candidates = context.candidates_filter;
            candidates.clear();
            candidates.emplace( //
                context.metric( //
                    new_node.vector, close_node.vector, new_node.head.dim, close_node.head.dim),
                new_id);
            iterate_through_neighbors(close_header, [&](id_t successor_id) noexcept {
                node_ref_t successor_node = node(successor_id);
                candidates.emplace( //
                    context.metric( //
                        successor_node.vector, close_node.vector, successor_node.head.dim, close_node.head.dim),
                    successor_id);
            });
            filter_top_candidates_with_heuristic(candidates, context.candidates_set, connectivity_max, context);

            // Export the results:
            close_header.count = 0u;
            while (candidates.size()) {
                close_header.neighbors[close_header.count] = candidates.top().second;
                close_header.count++;
                candidates.pop();
            }
        }

        return next_closest_entry_id;
    }

    level_t choose_random_level(std::default_random_engine& level_generator) const noexcept {
        std::uniform_real_distribution<double> distribution(0.0, 1.0);
        double r = -std::log(distribution(level_generator)) * pre_.inverse_log_connectivity;
        return (level_t)r;
    }

    void search_to_insert(                                         //
        id_t start_id, scalar_t const* query_vec, dim_t query_dim, //
        level_t level, thread_context_t& context) noexcept(false) {

        visits_bitset_t& visits = context.visits;
        distances_and_ids_t& top_candidates = context.top_candidates;
        distances_and_ids_t& candidates_set = context.candidates_set;

        top_candidates.clear();
        candidates_set.clear();
        visits.clear();

        distance_t closest_dist = context.metric(query_vec, node(start_id).vector, query_dim, node(start_id).head.dim);
        top_candidates.emplace(closest_dist, start_id);
        candidates_set.emplace(-closest_dist, start_id);
        visits.set(start_id);

        while (!candidates_set.empty()) {

            distance_and_id_t candidacy = candidates_set.top();
            if ((-candidacy.first) > closest_dist && top_candidates.size() == config_.expansion_add)
                break;

            candidates_set.pop();
            id_t candidate_id = candidacy.second;
            node_ref_t candidate_node = node(candidate_id);
            lock_t candidate_lock = candidate_node.lock();
            neighbors_ref_t candidate_header = neighbors(candidate_node, level);

            iterate_through_neighbors(candidate_header, [&](id_t successor_id) noexcept {
                if (visits.test(successor_id))
                    return;

                visits.set(successor_id);
                node_ref_t successor_node = node(successor_id);
                distance_t successor_dist =
                    context.metric(query_vec, successor_node.vector, query_dim, successor_node.head.dim);
                if (top_candidates.size() < config_.expansion_add || closest_dist > successor_dist) {
                    candidates_set.emplace(-successor_dist, successor_id);

                    top_candidates.emplace(successor_dist, successor_id);
                    if (top_candidates.size() > config_.expansion_add)
                        top_candidates.pop();
                    if (!top_candidates.empty())
                        closest_dist = top_candidates.top().first;
                }
            });
        }
    }

    void search_to_find_in_base(                                   //
        id_t start_id, scalar_t const* query_vec, dim_t query_dim, //
        std::size_t expansion, thread_context_t& context) const noexcept(false) {

        visits_bitset_t& visits = context.visits;
        distances_and_ids_t& top_candidates = context.top_candidates;
        distances_and_ids_t& candidates_set = context.candidates_set;

        visits.clear();
        top_candidates.clear();
        candidates_set.clear();

        distance_t closest_dist = context.metric(query_vec, node(start_id).vector, query_dim, node(start_id).head.dim);
        top_candidates.emplace(closest_dist, start_id);
        candidates_set.emplace(-closest_dist, start_id);
        visits.set(start_id);

        while (!candidates_set.empty()) {

            distance_and_id_t current_node_node = candidates_set.top();
            if ((-current_node_node.first) > closest_dist)
                break;

            candidates_set.pop();

            id_t candidate_id = current_node_node.second;
            neighbors_ref_t candidate_header = neighbors_base(node(candidate_id));

            iterate_through_neighbors(candidate_header, [&](id_t successor_id) noexcept {
                if (visits.test(successor_id))
                    return;

                visits.set(successor_id);
                node_ref_t successor_node = node(successor_id);
                distance_t successor_dist =
                    context.metric(query_vec, successor_node.vector, query_dim, successor_node.head.dim);

                if (top_candidates.size() < expansion || closest_dist > successor_dist) {
                    candidates_set.emplace(-successor_dist, successor_id);

                    top_candidates.emplace(successor_dist, successor_id);
                    if (top_candidates.size() > expansion)
                        top_candidates.pop();
                    if (!top_candidates.empty())
                        closest_dist = top_candidates.top().first;
                }
            });
        }
    }

    /**
     *  @brief A simple `for`-loop that prefetches vectors of neighbors.
     */
    template <typename neighbor_id_callback_at>
    inline void iterate_through_neighbors(neighbors_ref_t head, neighbor_id_callback_at&& callback) const noexcept {
        std::size_t n = head.count;
        for (std::size_t j = 0; j != n; j++)
            callback(head.neighbors[j]);
    }

    void filter_top_candidates_with_heuristic( //
        distances_and_ids_t& top_candidates, distances_and_ids_t& temporary, std::size_t needed,
        thread_context_t& context) const noexcept(false) {

        if (top_candidates.size() < needed)
            return;

        // TODO: Sort ascending, then run an inplace triangular reduction.
        temporary.clear();
        while (top_candidates.size()) {
            temporary.emplace(-top_candidates.top().first, top_candidates.top().second);
            top_candidates.pop();
        }

        while (temporary.size() && top_candidates.size() < needed) {

            distance_and_id_t best = temporary.top();
            distance_t dist_to_query = -best.first;
            temporary.pop();
            bool good = true;

            distance_and_id_t const* const top_unordered = top_candidates.data();
            std::size_t const top_count = top_candidates.size();
            for (std::size_t idx = 0; idx < top_count; idx++) {
                distance_and_id_t other = top_unordered[idx];
                node_ref_t other_node = node(other.second);
                node_ref_t best_node = node(best.second);
                distance_t inter_result_dist =
                    context.metric(other_node.vector, best_node.vector, other_node.head.dim, best_node.head.dim);
                if (inter_result_dist < dist_to_query) {
                    good = false;
                    break;
                }
            }

            if (good)
                top_candidates.emplace(-best.first, best.second);
        }
    }
};

} // namespace usearch
} // namespace unum
