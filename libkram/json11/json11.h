/* json11
 *
 * json11 is a tiny JSON library for C++11, providing JSON parsing and serialization.
 *
 * The core object provided by the library is json11::Json. A Json object represents any JSON
 * value: null, bool, number (int or double), string (string), array (vector), or
 * object (map).
 *
 * Json objects act like values: they can be assigned, copied, moved, compared for equality or
 * order, etc. There are also helper methods Json::dump, to serialize a Json to a string, and
 * Json::parse (static) to parse a string as a Json object.
 *
 * Internally, the various types of Json object are represented by the JsonValue class
 * hierarchy.
 *
 * A note on numbers - JSON specifies the syntax of number formatting but not its semantics,
 * so some JSON implementations distinguish between integers and floating-point numbers, while
 * some don't. In json11, we choose the latter. Because some JSON implementations (namely
 * Javascript itself) treat all numbers as the same type, distinguishing the two leads
 * to JSON that will be *silently* changed by a round-trip through those implementations.
 * Dangerous! To avoid that risk, json11 stores all numbers as double internally, but also
 * provides integer helpers.
 *
 * Fortunately, double-precision IEEE754 ('double') can precisely store any integer in the
 * range +/-2^53, which includes every 'int' on most systems. (Timestamps often use int64
 * or long long to avoid the Y2038K problem; a double storing microseconds since some epoch
 * will be exact for +/- 275 years.)
 */

/* Copyright (c) 2013 Dropbox, Inc.
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
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#pragma once

#include "KramConfig.h"

#include "ImmutableString.h"

namespace json11 {

using namespace NAMESPACE_STL;
using namespace kram;

class Json;
class JsonReaderData;

//--------------------------

// Write json nodes out to a string.  String data is encoded.
class JsonWriter final {
public:
    // Serialize.
    // caller must clear string.
    // TODO: accumulating to a string for large amounts of json is bad,
    // consider a real IO path use FileHelper or something.
    void write(const Json& root, string& out);
   
private:
    void write(const Json& root);
    
    void writeObject(const Json &values);
    void writeArray(const Json &values);
        
    void writeString(const Json &value);
    void writeNumber(const Json &value);
    void writeBool(const Json &value);
    void writeNull();
        
    // This could write to a FILE* instead of the string
    void writeText(const char* str);
    
private:
    string* _out = nullptr;
};

//--------------------------

// DOM-based parser with nice memory characteristics and small API.
class JsonReader final {
public:
    JsonReader();
    ~JsonReader();
    
    // Parse. If parse fails, return Json() and assign an error message to err.
    // Strings are aliased out of the incoming buffer. Keys are aliased
    // from an immutable pool.  And json nodes are allocated from a block
    // linear allocator.  So the returned Json only lives while reader does.
    Json* read(const char* str, uint32_t strCount);
    const string& error() const { return err; }
    
    void resetAndFree();
    size_t memoryUse() const;
    
    ImmutableString getImmutableKey(const char* key);
    
    // TODO: add call to decode and allocate all strings in tree.  Would allow mmap to be released.
    // should this have a string allocator, or use the existing block allocator?
    
private:
    void fail(const string& msg);
    
    void consume_whitespace();
    bool consume_comment();
    void consume_garbage();
    char get_next_token();
    bool compareStringForLength(const char* expected, size_t length);
    bool expect(const char* expected);
    
    void parse_string_location(uint32_t& count);
    double parse_number();
    ImmutableString parse_key();
    void parse_json(int depth, Json& parent, ImmutableString key = nullptr);
       
private:
    // State
    const char* str = nullptr;
    size_t strSize  = 0;
    size_t i = 0; // iterates through str, poor choice for member variable
    
    // error state
    string err;
    bool failed = false;
    uint32_t lineCount = 1; // lines are 1 based
    
    // parser is recursive instead of iterative, so has max depth to prevent runaway parsing.
    uint32_t maxDepth = 200;
    
    // allocator and immutable string pool are here
    unique_ptr<JsonReaderData> _data;
};

//--------------------------

// Json value type.  This is a tree of nodes with iterators and search.
class Json final {
public:
    // iterator to simplify walking lists/objects
    class const_iterator final
    {
    public:
        const_iterator(const Json* node) : _curr(node) { }
  
        const_iterator& operator=(const Json* node) {
            _curr = node; 
            return *this;
        }
  
        // Prefix ++ overload
        const_iterator& operator++() {
            if (_curr)
                _curr = _curr->_next;
            return *this;
        }
  
        // Postfix ++ overload
        const_iterator operator++(int)
        {
            const_iterator iterator = *this;
            ++(*this);
            return iterator;
        }
  
        bool operator==(const const_iterator& iterator) const { return _curr == iterator._curr; }
        bool operator!=(const const_iterator& iterator) const { return _curr != iterator._curr; }
        const Json& operator*() const { return *_curr; }
  
    private:
        const Json* _curr;
    };
    
    // Type
    enum Type : uint8_t {
        TypeNull,
        TypeNumber,
        TypeBoolean,
        TypeString,
        TypeArray,
        TypeObject
    };
    
    // Flags for additional data on a type
    enum Flags : uint8_t {
        FlagsNone = 0,
        FlagsAliasedEncoded, // needs decode on read
        FlagsAllocatedUnencoded, // needs encode on write
    };
    
    // Array/object can pass in for writer, but converted to linked nodes
    using array = vector<Json>;
    
    // Constructors for the various types of JSON value.
    Json() noexcept                  {}
    Json(nullptr_t) noexcept         {}
    Json(double value)               : _type(TypeNumber), _value(value) {}
    Json(int value)                  : Json((double)value) {}
    Json(bool value)                 : _type(TypeBoolean), _value(value) {}
    
    Json(const string& value)        : Json(value.c_str(), value.size())  {}
    Json(const char* value, uint32_t count_, bool allocated = true)
        : _type(TypeString), _flags(allocated ? FlagsAllocatedUnencoded : FlagsAliasedEncoded), _count(count_),
          _value(value, count_, allocated)
    { 
        // if (allocated) trackMemory(_count);
    }

    // This prevents Json(some_pointer) from accidentally producing a bool. Use
    // Json(bool(some_pointer)) if that behavior is desired.
    Json(void *) = delete;
    
    // has to recursively copy the entire tree of nodes, TODO:
    Json(const array& values, Type type = TypeArray);
    
    ~Json();
    
    /* Don't know if these can work
    // Implicit constructor: anything with a to_json() function.
    template <class T, class = decltype(&T::to_json)>
    Json(const T & t) : Json(t.to_json()) {}

    // Implicit constructor: map-like objects (map, unordered_map, etc)
    // TODO: revisit, but flatten objects to arrays
//    template <class M, typename enable_if<
//        is_constructible<string, decltype(declval<M>().begin()->first)>::value
//        && is_constructible<Json, decltype(declval<M>().begin()->second)>::value,
//            int>::type = 0>
//    Json(const M & m) : Json(object(m.begin(), m.end())) {}

    // Implicit constructor: vector-like objects (list, vector, set, etc)
    template <class V, typename enable_if<
        is_constructible<Json, decltype(*declval<V>().begin())>::value,
            int>::type = 0>
    Json(const V & v) : Json(array(v.begin(), v.end())) {}
    */
    
    // Accessors
    Type type() const { return _type; }
    
    // Only for object type, caller can create from JsonReader
    ImmutableString key() const;
    void setKey(ImmutableString key);
    
    // array/objects have count and iterate call
    size_t count() const { return _count; };
    // Return a reference to arr[i] if this is an array, Json() otherwise.
    const Json & operator[](uint32_t i) const;
    // Return a reference to obj[key] if this is an object, Json() otherwise.
    const Json & operator[](const char* key) const;
    
    // implement standard iterator paradigm for linked list
    const_iterator begin() const { assert(is_array() || is_object()); return const_iterator(_value.aval); }
    const_iterator end() const { return const_iterator(nullptr); }
    
    bool iterate(const Json*& it) const;
    
    bool is_null()   const  { return _type == TypeNull; }
    bool is_number() const  { return _type == TypeNumber; }
    bool is_boolean() const { return _type == TypeBoolean; }
    bool is_string() const  { return _type == TypeString; }
    bool is_array()  const  { return _type == TypeArray; }
    bool is_object() const  { return _type == TypeObject; }

    // Return the enclosed value if this is a number, 0 otherwise. Note that json11 does not
    // distinguish between integer and non-integer numbers - number_value() and int_value()
    // can both be applied to a NUMBER-typed object.
    double number_value() const { return is_number() ? _value.dval : 0.0; }
    float double_value() const { return number_value(); }
    float float_value() const { return (float)number_value(); }
    int int_value() const { return (int)number_value(); }
    
    // Return the enclosed value if this is a boolean, false otherwise.
    bool boolean_value() const { return is_boolean() ? _value.bval : false; }
    // Return the enclosed string if this is a string, empty string otherwise
    const char* string_value(string& str) const;

    // TODO: do we really need these comparisons?, typically just doing a key search
    // only have to implement 2 operators
    //bool operator== (const Json &rhs) const;
    //bool operator<  (const Json &rhs) const;
    //bool operator!= (const Json &rhs) const { return !(*this == rhs); }
//    bool operator<= (const Json &rhs) const { return !(rhs < *this); }
//    bool operator>  (const Json &rhs) const { return  (rhs < *this); }
//    bool operator>= (const Json &rhs) const { return !(*this < rhs); }

    // Return true if this is a JSON object and, for each item in types, has a field of
    // the given type. If not, return false and set err to a descriptive message.
    // typedef std::initializer_list<pair<string, Type>> shape;
    // bool has_shape(const shape & types, string & err) const;

    // quickly find a node using immutable string
    const Json & find(ImmutableString key) const;

    // useful for deleting allocated string values in block allocated nodes
    // so it does a placement delete
    // void deleteJsonTree();
   
private:
    friend class JsonReader;
    
    // Doesn't seem to work with namespaced class
    void createRoot();

    // TODO: make need to expose to build up a json hierarchy for dumping
    void addJson(Json* json);
    void addString(Json* json, const char* str, uint32_t len, Flags flags, ImmutableString key = nullptr);
    void addNull(Json* json, ImmutableString key = nullptr);
    void addBoolean(Json* json, bool b, ImmutableString key = nullptr);
    void addNumber(Json* json, double number, ImmutableString key = nullptr);
    void addArray(Json* json, ImmutableString key = nullptr);
    void addObject(Json* json,ImmutableString key = nullptr);

private:
    void trackMemory(int32_t size);

    // This type is 32B / node w/key ptr, w/2B key it's 24B / node.
    
    // 8B - objects store key in children
    // debugging difficult without key as string
    const char* _key = nullptr;
    
    // 2B, but needs lookup table then
    //uint16_t _key = 0;
    uint16_t _padding = 0;
    
    // 1B - really 3 bits
    Type _type = TypeNull;
    
    // 1B - really 1-2 bits
    Flags _flags = FlagsNone;
    
    // 4B - count used by array/object, also by string
    uint32_t _count = 0;
    
    // 8B - value to hold double and ptrs
    union JsonValue {
        JsonValue() : aval(nullptr) { }
        JsonValue(double v) : dval(v) {}
        JsonValue(bool v) : bval(v) {}
        JsonValue(const char* v, uint32_t count, bool allocate);
        JsonValue(const Json::array& value, Type t = TypeArray);
        
        // allocated strings deleted by Json dtor which knows type
        // the rest are all just block allocated
        
        double     dval;
        bool       bval;
        
        // 2 string forms - aliased to mmap (terminated with ", not-escaped)
        // not-escaped and allocated which is null terminated
        const char* sval;
        
        //uint32_t aval;
        Json* aval; // aliased children, chained with _next to form tree
    } _value;
    
    // 8B - arrays/object chain values, so this is non-null on more than just array/object type
    // aval is the root of the children.
    //uint32_t _next = 0;
    Json* _next = nullptr;
};

} // namespace json11