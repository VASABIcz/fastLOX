// Michal Vlasák, FIT CTU, 2024

// This is a sample implementation of a hash table, mostly based on the one from
// the Crafting interpreters book [1]. This one is fancy, because it is generic
// and in C++. It provides basic `get` and `insert` methods, and doesn't provide
// deletion. If you need more, you may find it in the book, or implement it
// yourself - it's just an open addressing hash table with linear probing.
//
// The hash table assumes a few things:
//
// (1) A default constructed key (i.e. `K()`) can be used as a special dummy
//     value in the hash table, which means no entry present. Such keys are
//     expected to never be used in calls to `get` or `insert`, etc.
//
// (2) `std::hash<K>` can be used to compute hash of the key.
//
// (3) `operator ==` can be used to compare the keys.
//
// Example use is below as a test case. Notice how using the value semantics for
// the keys makes it not nice to have reference types (pointer types that should
// delegate their behaviour to the pointed to values) for keys. You can
// customize the implementatiaon here for your convenience and depending on your
// actual implementation details (e.g. also not making it generic makes it
// easier).
//
// The hash table just allocates with the allocating function (required as
// parameter for `insert`), though is able to use `malloc` as the default. Again
// this is something you may want to simplify as appropriate.
//
//
// You may be interested in following things to try:
//
// (1) A more crude approach to hash tables and double hashing [2].
// (2) Storing the hash in the entries/keys, to not recompute it all the time.
// (3) Hettinger's "Compact dictionary" approach [3, 4], which doesn't have
//     holes in the Entry array, and uses an array of "indices". This approach
//     is sometimes called "index map".
//
//
// [1]: https://craftinginterpreters.com/hash-tables.html
// [2]: https://nullprogram.com/blog/2022/08/08/
// [3]: https://mail.python.org/pipermail/python-dev/2012-December/123028.html
// [4]: https://blog.toit.io/hash-maps-that-dont-hate-you-1a96150b492a

#include <cstddef>
#include <cstring>
#include <functional>
#include <optional>

template <typename K, typename V>
class Table {
public:
	Table() : entries(nullptr), entry_cnt(0), capacity(0) {}

	[[nodiscard]] std::optional<V> get(K key) const {
		if (entry_cnt == 0) {
			return std::nullopt;
		}
		Entry *entry = find_entry(entries, capacity, key);
		if (entry->key == K()) {
			return std::nullopt;
		}
		return entry->value;
	}

	// Returns true if the entry was newly inserted
	template <typename F>
	bool insert(K key, V value, F alloc_function) {
		if (entry_cnt >= capacity / 2) {
			grow(alloc_function);
		}
		Entry *entry = find_entry(entries, capacity, key);
		bool is_new = entry->key == K();
		if (is_new) {
			entry_cnt += 1;
			entry->key = key;
		}
		entry->value = value;
		return is_new;
	}

	bool insert(K key, V value) {
		return insert(key, value, malloc);
	}


	size_t size() {
		return entry_cnt;
	}

private:
	struct Entry {
		K key;
		V value;
	};

	template <typename F>
	void grow(F alloc_function) {
		size_t new_capacity = capacity ? capacity * 2 : 8;
		// NOTE: We depend on memory being zero initialized here.
		auto *new_entries = (Entry *) alloc_function(new_capacity * sizeof(entries[0]));
		memset(new_entries, 0, new_capacity * sizeof(entries[0]));
		for (size_t i = 0; i < capacity; i++) {
			Entry *old_entry = &entries[i];
			if (old_entry->key == K()) {
				continue;
			}
			Entry *new_entry = find_entry(new_entries, new_capacity, old_entry->key);
			*new_entry = *old_entry;
		}
		entries = new_entries;
		capacity = new_capacity;
	}

	static Entry *find_entry(Entry *entries, size_t capacity, K key) {
		size_t hash = std::hash<K>()(key);
		size_t mask = capacity - 1;
		for (size_t index = hash & mask;; index = (index + 1) & mask) {
			Entry *entry = &entries[index];
			if (entry->key == K() || entry->key == key) {
				return entry;
			}
		}
	}

	Entry *entries;
	size_t entry_cnt;
	size_t capacity;
};

// Here's a trick. If this file is not included as a header file, but compiled
// directly, then we include a test as a main function. So normally including
// the file still works, we just allow to test this file with:
//
//     g++ -x c++ -std=gnu++20 table.hpp -o table_test && ./table_test
//
// (It is necessary to specify `-x c++`, otherwise gcc attempts to precompile
// the header file, which is not what we want.)

#if __INCLUDE_LEVEL__ == 0

#include <cassert>
#include <cstring>

#include "allocator.hpp"

struct MyStringImpl {
	MyStringImpl(const char *str, size_t length) : str(str), length(length) {}

	const char *str;
	size_t length;

	bool operator==(MyStringImpl const &other) const {
		return length == other.length && memcmp(str, other.str, length) == 0;
	}
};

struct MyString {
	MyString() : pimpl(nullptr) {}
	MyString(MyStringImpl *pimpl) : pimpl(pimpl) {}
	MyStringImpl *pimpl;

	bool operator==(MyString const &other) const {
		if (pimpl && other.pimpl) {
			return *pimpl == *other.pimpl;
		} else {
			return pimpl == other.pimpl;
		}
	}
};


// FNV-1a
// https://en.wikipedia.org/wiki/Fowler%E2%80%93Noll%E2%80%93Vo_hash_function
template<>
struct std::hash<MyString>
{
    std::size_t operator()(MyString const& str) const noexcept
    {
	size_t h = 14695981039346656037ULL;
	for (size_t i = 0; i < str.pimpl->length; i++) {
		// beware of unwanted sign extension!
		h ^= (unsigned char) str.pimpl->str[i];
		h *= 1099511628211;
	}
	return h;

    }
};

struct MyValue {
	int x;
};


int main() {
	Table<MyString, MyValue *> table;

	ArenaAllocator heap(MemoryKind_Heap);

	VMLogger::get().log("Init START");

	MyString (k[9]);
	k[0] = heap.create<MyStringImpl>("k1", 2);
	k[1] = heap.create<MyStringImpl>("key2", 4);
	k[2] = heap.create<MyStringImpl>("key 3", 5);
	k[3] = heap.create<MyStringImpl>("key 4 __ ", 9);
	k[4] = heap.create<MyStringImpl>("", 0);
	k[5] = heap.create<MyStringImpl>("MyKey6", 6);
	k[6] = heap.create<MyStringImpl>("  ", 2);
	k[7] = heap.create<MyStringImpl>(" ", 1);
	k[8] = heap.create<MyStringImpl>("nine", 4);

	auto alloc = [&heap] (size_t nbytes) {
		return heap.alloc(nbytes, sizeof(max_align_t));
	};

	MyValue *(v[sizeof(k) / sizeof(k[0])]);
	for (size_t i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
		v[i] = heap.create<MyValue>(i);
	}

	VMLogger::get().log("Init END");

	VMLogger::get().log("Test START");

	assert(table.size() == 0);
	assert(table.insert(k[0], v[0], alloc));
	assert(table.get(k[0]) && *table.get(k[0]) == v[0]);
	assert(table.size() == 1);

	assert(!table.insert(k[0], v[1], alloc));
	assert(table.get(k[0]) && *table.get(k[0]) == v[1]);
	assert(table.size() == 1);

	assert(table.insert(k[1], v[1], alloc));
	assert(table.get(k[0]) && *table.get(k[0]) == v[1]);
	assert(table.get(k[1]) && *table.get(k[1]) == v[1]);
	assert(table.size() == 2);

	assert(!table.insert(k[0], v[0], alloc));
	assert(table.get(k[0]) && *table.get(k[0]) == v[0]);
	assert(table.get(k[1]) && *table.get(k[1]) == v[1]);
	assert(table.size() == 2);

	for (size_t i = 2; i < sizeof(k) / sizeof(k[0]); i++) {
		assert(table.insert(k[i], v[i], alloc));
		assert(table.get(k[i]) && *table.get(k[i]) == v[i]);
		assert(table.size() == i + 1);
		assert(!table.insert(k[i], v[i], alloc));
		assert(table.size() == i + 1);
	}

	MyString key_copy = k[0];
	assert(table.get(key_copy) && *table.get(key_copy) == v[0]);
	key_copy.pimpl->str = "k2";
	assert(!table.get(key_copy));
	assert(!table.get(k[0]));
	key_copy.pimpl->str = "k1";
	assert(table.get(key_copy) && *table.get(key_copy) == v[0]);
	assert(table.get(k[0]) && *table.get(k[0]) == v[0]);
	key_copy.pimpl->str = "key2";
	key_copy.pimpl->length = 4;
	assert(table.get(k[1]) && *table.get(k[1]) == v[1]);
	assert(table.get(key_copy) && *table.get(key_copy) == v[1]);
	assert(table.get(k[0]) && *table.get(k[0]) == v[1]);
	key_copy.pimpl->str = "k1";
	key_copy.pimpl->length = 2;
	assert(table.get(k[1]) && *table.get(k[1]) == v[1]);
	assert(table.get(key_copy) && *table.get(key_copy) == v[0]);
	assert(table.get(k[0]) && *table.get(k[0]) == v[0]);

	for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
		assert(table.get(k[i]) && *table.get(k[i]) == v[i]);
		assert(!table.insert(k[i], v[0], alloc));
		assert(table.get(k[i]) && *table.get(k[i]) == v[0]);
	}

	VMLogger::get().log("Test END");
}

#endif
