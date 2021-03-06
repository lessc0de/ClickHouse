#pragma once

#include <DB/Dictionaries/IDictionary.h>
#include <DB/Dictionaries/IDictionarySource.h>
#include <DB/Dictionaries/DictionaryStructure.h>
#include <DB/Common/HashTable/HashMap.h>
#include <DB/Common/ArenaWithFreeLists.h>
#include <DB/Columns/ColumnString.h>
#include <ext/scope_guard.hpp>
#include <ext/bit_cast.hpp>
#include <ext/range.hpp>
#include <ext/size.hpp>
#include <ext/map.hpp>
#include <Poco/RWLock.h>
#include <cmath>
#include <atomic>
#include <chrono>
#include <vector>
#include <map>
#include <tuple>
#include <random>


namespace DB
{

class CacheDictionary final : public IDictionary
{
public:
	CacheDictionary(const std::string & name, const DictionaryStructure & dict_struct,
		DictionarySourcePtr source_ptr, const DictionaryLifetime dict_lifetime,
		const std::size_t size);

	CacheDictionary(const CacheDictionary & other);

	std::exception_ptr getCreationException() const override { return {}; }

	std::string getName() const override { return name; }

	std::string getTypeName() const override { return "Cache"; }

	std::size_t getBytesAllocated() const override { return bytes_allocated + (string_arena ? string_arena->size() : 0); }

	std::size_t getQueryCount() const override { return query_count.load(std::memory_order_relaxed); }

	double getHitRate() const override
	{
		return static_cast<double>(hit_count.load(std::memory_order_acquire)) /
			query_count.load(std::memory_order_relaxed);
	}

	std::size_t getElementCount() const override { return element_count.load(std::memory_order_relaxed); }

	double getLoadFactor() const override
	{
		return static_cast<double>(element_count.load(std::memory_order_relaxed)) / size;
	}

	bool isCached() const override { return true; }

	DictionaryPtr clone() const override { return std::make_unique<CacheDictionary>(*this); }

	const IDictionarySource * getSource() const override { return source_ptr.get(); }

	const DictionaryLifetime & getLifetime() const override { return dict_lifetime; }

	const DictionaryStructure & getStructure() const override { return dict_struct; }

	std::chrono::time_point<std::chrono::system_clock> getCreationTime() const override
	{
		return creation_time;
	}

	bool isInjective(const std::string & attribute_name) const override
	{
		return dict_struct.attributes[&getAttribute(attribute_name) - attributes.data()].injective;
	}

	bool hasHierarchy() const override { return hierarchical_attribute; }

	void toParent(const PaddedPODArray<id_t> & ids, PaddedPODArray<id_t> & out) const override;

#define DECLARE(TYPE)\
	void get##TYPE(const std::string & attribute_name, const PaddedPODArray<id_t> & ids, PaddedPODArray<TYPE> & out) const;
	DECLARE(UInt8)
	DECLARE(UInt16)
	DECLARE(UInt32)
	DECLARE(UInt64)
	DECLARE(Int8)
	DECLARE(Int16)
	DECLARE(Int32)
	DECLARE(Int64)
	DECLARE(Float32)
	DECLARE(Float64)
#undef DECLARE

	void getString(const std::string & attribute_name, const PaddedPODArray<id_t> & ids, ColumnString * out) const;

#define DECLARE(TYPE)\
	void get##TYPE(\
		const std::string & attribute_name, const PaddedPODArray<id_t> & ids, const PaddedPODArray<TYPE> & def,\
		PaddedPODArray<TYPE> & out) const;
	DECLARE(UInt8)
	DECLARE(UInt16)
	DECLARE(UInt32)
	DECLARE(UInt64)
	DECLARE(Int8)
	DECLARE(Int16)
	DECLARE(Int32)
	DECLARE(Int64)
	DECLARE(Float32)
	DECLARE(Float64)
#undef DECLARE

	void getString(
		const std::string & attribute_name, const PaddedPODArray<id_t> & ids, const ColumnString * const def,
		ColumnString * const out) const;

#define DECLARE(TYPE)\
	void get##TYPE(\
		const std::string & attribute_name, const PaddedPODArray<id_t> & ids, const TYPE def, PaddedPODArray<TYPE> & out) const;
	DECLARE(UInt8)
	DECLARE(UInt16)
	DECLARE(UInt32)
	DECLARE(UInt64)
	DECLARE(Int8)
	DECLARE(Int16)
	DECLARE(Int32)
	DECLARE(Int64)
	DECLARE(Float32)
	DECLARE(Float64)
#undef DECLARE

	void getString(
		const std::string & attribute_name, const PaddedPODArray<id_t> & ids, const String & def,
		ColumnString * const out) const;

	void has(const PaddedPODArray<id_t> & ids, PaddedPODArray<UInt8> & out) const override;

private:
	template <typename Value> using MapType = HashMap<id_t, Value>;
	template <typename Value> using ContainerType = Value[];
	template <typename Value> using ContainerPtrType = std::unique_ptr<ContainerType<Value>>;

	struct cell_metadata_t final
	{
		using time_point_t = std::chrono::system_clock::time_point;
		using time_point_rep_t = time_point_t::rep;
		using time_point_urep_t = std::make_unsigned_t<time_point_rep_t>;

		static constexpr std::uint64_t EXPIRES_AT_MASK = std::numeric_limits<time_point_rep_t>::max();
		static constexpr std::uint64_t IS_DEFAULT_MASK = ~EXPIRES_AT_MASK;

		std::uint64_t id;
		/// Stores both expiration time and `is_default` flag in the most significant bit
		time_point_urep_t data;

		/// Sets expiration time, resets `is_default` flag to false
		time_point_t expiresAt() const { return ext::safe_bit_cast<time_point_t>(data & EXPIRES_AT_MASK); }
		void setExpiresAt(const time_point_t & t) { data = ext::safe_bit_cast<time_point_urep_t>(t); }

		bool isDefault() const { return (data & IS_DEFAULT_MASK) == IS_DEFAULT_MASK; }
		void setDefault() { data |= IS_DEFAULT_MASK; }
	};

	struct attribute_t final
	{
		AttributeUnderlyingType type;
		std::tuple<
			UInt8, UInt16, UInt32, UInt64,
			Int8, Int16, Int32, Int64,
			Float32, Float64,
			String> null_values;
		std::tuple<
			ContainerPtrType<UInt8>, ContainerPtrType<UInt16>, ContainerPtrType<UInt32>, ContainerPtrType<UInt64>,
			ContainerPtrType<Int8>, ContainerPtrType<Int16>, ContainerPtrType<Int32>, ContainerPtrType<Int64>,
			ContainerPtrType<Float32>, ContainerPtrType<Float64>,
			ContainerPtrType<StringRef>> arrays;
	};

	void createAttributes();

	attribute_t createAttributeWithType(const AttributeUnderlyingType type, const Field & null_value);


	template <typename OutputType, typename DefaultGetter>
	void getItemsNumber(
		attribute_t & attribute,
		const PaddedPODArray<id_t> & ids,
		PaddedPODArray<OutputType> & out,
		DefaultGetter && get_default) const;

	template <typename AttributeType, typename OutputType, typename DefaultGetter>
	void getItemsNumberImpl(
		attribute_t & attribute,
		const PaddedPODArray<id_t> & ids,
		PaddedPODArray<OutputType> & out,
		DefaultGetter && get_default) const;

	template <typename DefaultGetter>
	void getItemsString(
		attribute_t & attribute,
		const PaddedPODArray<id_t> & ids,
		ColumnString * out,
		DefaultGetter && get_default) const;

	template <typename PresentIdHandler, typename AbsentIdHandler>
	void update(
		const std::vector<id_t> & requested_ids, PresentIdHandler && on_cell_updated,
		AbsentIdHandler && on_id_not_found) const;

	std::uint64_t getCellIdx(const id_t id) const;

	void setDefaultAttributeValue(attribute_t & attribute, const id_t idx) const;

	void setAttributeValue(attribute_t & attribute, const id_t idx, const Field & value) const;

	attribute_t & getAttribute(const std::string & attribute_name) const;

	const std::string name;
	const DictionaryStructure dict_struct;
	const DictionarySourcePtr source_ptr;
	const DictionaryLifetime dict_lifetime;

	mutable Poco::RWLock rw_lock;
	const std::size_t size;
	const std::uint64_t zero_cell_idx{getCellIdx(0)};
	std::map<std::string, std::size_t> attribute_index_by_name;
	mutable std::vector<attribute_t> attributes;
	mutable std::vector<cell_metadata_t> cells;
	attribute_t * hierarchical_attribute = nullptr;
	std::unique_ptr<ArenaWithFreeLists> string_arena;

	mutable std::mt19937_64 rnd_engine;

	mutable std::size_t bytes_allocated = 0;
	mutable std::atomic<std::size_t> element_count{0};
	mutable std::atomic<std::size_t> hit_count{0};
	mutable std::atomic<std::size_t> query_count{0};

	const std::chrono::time_point<std::chrono::system_clock> creation_time = std::chrono::system_clock::now();
};

}
