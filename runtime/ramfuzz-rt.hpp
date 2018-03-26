// Copyright 2016-2018 The RamFuzz contributors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <functional>
#include <limits>
#include <ostream>
#include <random>
#include <sstream>
#include <string>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

#define UNW_LOCAL_ONLY
#include <libunwind.h>

namespace ramfuzz {

/// RamFuzz harness for testing C objects.
///
/// The harness class contains a pointer to C as a public member named obj.
/// There is an interface for invoking obj's methods with random parameters, as
/// described below.  The harness creates obj but does not own it; the client
/// code does.
///
/// The harness class has one method for each public non-static method of C.  A
/// harness method, when invoked, generates random arguments and invokes the
/// corresponding method under test.  Harness methods take no arguments, as they
/// are self-contained and generate random values internally.  Their return type
/// is void (except for constructors, as described below).
///
/// Each of C's public constructors also gets a harness method.  These harness
/// methods allocate a new C and invoke the corresponding C constructor.  They
/// return a pointer to the constructed object.
///
/// The count of constructor harness methods is kept in a member named ccount.
/// There is also a member named croulette; it's an array of ccount method
/// pointers, one for each constructor method.  The harness class itself has a
/// constructor that constructs a C instance using a randomly chosen C
/// constructor.  This constructor takes a runtime::gen reference as a
/// parameter.
///
/// The count of non-constructor harness methods is kept in a member named
/// mcount.  There is also a member named mroulette; it's an array of mcount
/// method pointers, one for each non-constructor harness method.
///
/// A member named subcount contains the number of C's direct subclasses.  A
/// member named submakers is an array of subcount pointers to functions of type
/// C*(runtime::gen&).  Each direct subclass D has a submakers element that
/// creates a random D object and returns a pointer to it.
template <class C> class harness;

namespace runtime {

/// The upper limit on how many times to spin the method roulette in generated
/// RamFuzz classes.  Should be defined in user's code.
extern unsigned spinlimit;

/// Exception thrown when there's a file-access error.
struct file_error : public std::runtime_error {
  explicit file_error(const std::string &s) : runtime_error(s) {}
  explicit file_error(const char *s) : runtime_error(s) {}
};

/// Returns T's type tag to put into RamFuzz logs.
template <typename T> char typetag(T);

/// Generates values for RamFuzz code.  Can be used in the "generate" or
/// "replay" mode.  In "generate" mode, values are created at random and logged.
/// In "replay" mode, values are read from a previously generated log.  This
/// allows debugging of failed tests.
///
/// Depends on test code that RamFuzz generates (see ../main.cpp) -- in fact,
/// the generated fuzz.hpp file contains `#include "ramfuzz-rt.hpp"`, because
/// they'll always be used together.
///
/// It is recommended to use the same gen object for generating all parameters
/// in one test run.  That captures them all in the log file, so the test can be
/// easily replayed, and the log can be processed by AI tools in ../ai.  See
/// also the constructor gen(argc, argv, k) below.
///
/// The log is in binary format, to ensure replay precision.  Each log entry
/// contains the value generated and an ID for that value.  The ID is currently
/// based on the program's execution state, indicating the program location at
/// which the value is generated.  Different program runs may generate different
/// values at the same location; this is useful for AI analysis of the logs and
/// program outcomes.
class gen {
  /// Are we generating values or replaying a previous run?
  enum { generate, replay } runmode;

public:
  /// Values will be generated and logged in ologname.
  gen(const std::string &ologname = "fuzzlog");

  /// Values will be replayed from ilogname and logged into ologname.
  gen(const std::string &ilogname, const std::string &ologname);

  /// Interprets kth command-line argument.  If the argument exists (ie, k <
  /// argc), values will be replayed from file named argv[k] and logged in
  /// argv[k]+"+".  If the argument doesn't exist, values will be generated and
  /// logged in "fuzzlog".
  ///
  /// This makes it convenient for main(argc, argv) to invoke gen(argc, argv),
  /// yielding a program that either generates its values (if no command-line
  /// arguments) or replays the log file named by its first argument.
  gen(int argc, const char *const *argv, size_t k = 1);

  /// Returns an unconstrained value of type T and logs it.  The value is random
  /// in "generate" mode but read from the input log in "replay" mode.
  ///
  /// If allow_subclass is true, the result may be an object of T's subclass.
  template <typename T> T *make(size_t valueid, bool allow_subclass = false) {
    auto &oldies = storage[std::type_index(typeid(T))];
    if (!oldies.empty() && reuse())
      // Note we don't check allow_subclass here, so T's storage must never hold
      // subclass objects, only actual Ts.
      return reinterpret_cast<T *>(
          oldies[between<size_t>(0, oldies.size() - 1, valueid)]);
    else
      return makenew<T>(valueid, allow_subclass);
  }

  /// Handy name for invoking make<T>(or_subclass).
  static constexpr bool or_subclass = true;

  /// Returns a value of numeric type T between lo and hi, inclusive, and logs
  /// it.  The value is random in "generate" mode but read from the input log in
  /// "replay" mode.
  template <typename T> T between(T lo, T hi, size_t valueid) {
    T val;
    if (runmode == generate)
      val = uniform_random(lo, hi);
    else
      input(val);
    output(val, valueid);
    return val;
  }

private:
  /// Logs val and id to olog.
  template <typename U> void output(U val, size_t id) {
    olog.put(typetag(val));
    olog.write(reinterpret_cast<char *>(&val), sizeof(val));
    olog.write(reinterpret_cast<char *>(&id), sizeof(id));
    olog.flush();
  }

  /// Reads val from ilog and advances ilog to the beginning of the next value.
  template <typename T> void input(T &val) {
    const char ty = ilog.get();
    assert(ty == typetag(val));
    ilog.read(reinterpret_cast<char *>(&val), sizeof(val));
    size_t id;
    ilog.read(reinterpret_cast<char *>(&id), sizeof(id));
  }

  /// Stores p as the newest element in T's storage.  Returns p.
  template <typename T> T *store(T *p) {
    storage[std::type_index(typeid(T))].push_back(p);
    return p;
  }

  /// Provides a static const member named `value` that's true iff T is a char*
  /// (modulo const/volatile).
  template <typename T> struct is_char_ptr {
    static const auto value =
        std::is_pointer<T>::value &&
        std::is_same<char, typename std::remove_cv<typename std::remove_pointer<
                               T>::type>::type>::value;
  };

  /// Like the public make(), but creates a brand new object and never returns
  /// previously created ones.
  ///
  /// There are several overloads for different kinds of T: arithmetic types,
  /// classes, pointers, etc.
  template <typename T>
  T *makenew(size_t valueid,
             typename std::enable_if<std::is_arithmetic<T>::value ||
                                         std::is_enum<T>::value,
                                     bool>::type allow_subclass = false) {
    return store(new T(between(std::numeric_limits<T>::min(),
                               std::numeric_limits<T>::max(), valueid)));
  }

  template <typename T>
  T *makenew(size_t valueid,
             typename std::enable_if<std::is_class<T>::value ||
                                         std::is_union<T>::value,
                                     bool>::type allow_subclass = false) {
    if (harness<T>::subcount && allow_subclass &&
        between(0., 1., valueid) > 0.5) {
      return (*harness<T>::submakers[between(
          size_t{0}, harness<T>::subcount - 1, valueid)])(*this);
    } else {
      harness<T> h(*this);
      if (h.mcount) {
        for (auto i = 0u, e = between(0u, runtime::spinlimit, valueid); i < e;
             ++i)
          (h.*h.mroulette[between(0u, h.mcount - 1, valueid)])();
      }
      return store(h.obj);
    }
  }

  template <typename T>
  T *makenew(
      size_t valueid,
      typename std::enable_if<std::is_void<T>::value, bool>::type = false) {
    return store<void>(new char[between(1, 4196, valueid)]);
  }

  template <typename T>
  T *makenew(size_t valueid,
             typename std::enable_if<std::is_pointer<T>::value &&
                                         !is_char_ptr<T>::value,
                                     bool>::type allow_subclass = false) {
    using pointee = typename std::remove_pointer<T>::type;
    return store(new T(
        make<typename std::remove_cv<pointee>::type>(valueid, allow_subclass)));
  }

  /// Most of the time, char* should be a null-terminated string, so it gets its
  /// own overload.
  template <typename T>
  T *makenew(size_t valueid,
             typename std::enable_if<is_char_ptr<T>::value, bool>::type
                 allow_subclass = false) {
    auto r = new char *;
    const auto sz = *make<size_t>(1000000 + valueid);
    *r = new char[sz + 1];
    (*r)[sz] = '\0';
    for (size_t i = 0; i < sz; ++i)
      (*r)[i] = between(std::numeric_limits<char>::min(),
                        std::numeric_limits<char>::max(), valueid);
    return const_cast<T *>(r);
  }

  template <typename T>
  T *makenew(size_t valueid,
             typename std::enable_if<std::is_function<T>::value, bool>::type
                 allow_subclass = false) {
    // TODO: implement.  Either capture \c this somehow to make() a value of the
    // return type; or select randomly one of existing functions in the program
    // that fit the signature.
    return 0;
  }

  /// Returns a random value distributed uniformly between lo and hi, inclusive.
  /// Logs the value in olog.
  template <typename T> T uniform_random(T lo, T hi);

  /// Whether make() should reuse a previously created value or create a fresh
  /// one.  Decided randomly.
  bool reuse() { return false /*between(false, true)*/; }

  /// Used for random value generation.
  std::ranlux24 rgen = std::ranlux24(std::random_device{}());

  /// Output log.
  std::ofstream olog;

  /// Input log in replay mode.
  std::ifstream ilog;

  /// Stores all values generated by makenew().
  std::unordered_map<std::type_index, std::vector<void *>> storage;

  /// A reference PC (program counter) value.  All PC values calculated by
  /// valueid() will be relative to this value, which will make them
  /// position-independent.
  unw_word_t base_pc;
};

/// Limit on the call-stack depth in generated RamFuzz methods.  Without such a
/// limit, infinite recursion is possible for certain code under test (eg,
/// ClassA::method1(B b) and ClassB::method2(A a)).  The user can modify this
/// value or the depthlimit member of any RamFuzz class.
constexpr unsigned depthlimit = 20;

} // namespace runtime

template <> class harness<std::exception> {
public:
  std::exception *obj;
  harness(runtime::gen &) : obj(new std::exception) {}
  operator bool() const { return true; }
  using mptr = void (harness::*)();
  static constexpr unsigned mcount = 0;
  static constexpr mptr mroulette[] = {};
  static constexpr unsigned ccount = 1;
  static constexpr size_t subcount = 0;
  static constexpr std::exception *(*submakers[])(runtime::gen &) = {};
};

template <typename Tp, typename Alloc> class harness<std::vector<Tp, Alloc>> {
private:
  // Declare first to initialize early; constructors may use it.
  runtime::gen &g;
  using Vec = std::vector<Tp, Alloc>;

public:
  Vec *obj;

  harness(runtime::gen &g)
      : g(g), obj(new Vec(*g.make<typename Vec::size_type>(1))) {
    for (size_t i = 0; i < obj->size(); ++i)
      (*obj)[i] = *g.make<typename std::remove_cv<Tp>::type>(2);
  }

  operator bool() const { return true; }
  using mptr = void (harness::*)();
  static constexpr unsigned mcount = 0;
  static constexpr mptr mroulette[] = {};
  static constexpr unsigned ccount = 1;
  static constexpr size_t subcount = 0;
  static constexpr Vec *(*submakers[])(runtime::gen &) = {};
};

template <class CharT, class Traits, class Allocator>
class harness<std::basic_string<CharT, Traits, Allocator>> {
private:
  // Declare first to initialize early; constructors may use it.
  runtime::gen &g;

public:
  std::basic_string<CharT, Traits, Allocator> *obj;
  harness(runtime::gen &g)
      : g(g), obj(new std::basic_string<CharT, Traits, Allocator>(
                  *g.make<size_t>(3), CharT())) {
    for (size_t i = 0; i < obj->size() - 1; ++i)
      obj[i] = g.between<CharT>(1, std::numeric_limits<CharT>::max(), 4);
    obj->back() = CharT(0);
  }
  operator bool() const { return true; }
  using mptr = void (harness::*)();
  static constexpr unsigned mcount = 0;
  static constexpr mptr mroulette[] = {};
  static constexpr unsigned ccount = 1;
  static constexpr size_t subcount = 0;
  static constexpr std::basic_string<CharT, Traits, Allocator> *(*submakers[])(
      runtime::gen &) = {};
};

template <class CharT, class Traits>
class harness<std::basic_istream<CharT, Traits>> {
  // Declare first to initialize early; constructors may use it.
  runtime::gen &g;

public:
  std::basic_istringstream<CharT, Traits> *obj;
  harness(runtime::gen &g)
      : g(g), obj(new std::basic_istringstream<CharT, Traits>(
                  *g.make<std::string>(5))) {}
  operator bool() const { return true; }
  using mptr = void (harness::*)();
  static constexpr unsigned mcount = 0;
  static constexpr mptr mroulette[] = {};
  static constexpr unsigned ccount = 1;
  static constexpr size_t subcount = 0;
  static constexpr std::basic_istream<CharT, Traits> *(*submakers[])(
      runtime::gen &) = {};
};

template <class CharT, class Traits>
class harness<std::basic_ostream<CharT, Traits>> {
public:
  std::basic_ostringstream<CharT, Traits> *obj;
  harness(runtime::gen &g) : obj(new std::basic_ostringstream<CharT, Traits>) {}
  operator bool() const { return true; }
  using mptr = void (harness::*)();
  static constexpr unsigned mcount = 0;
  static constexpr mptr mroulette[] = {};
  static constexpr unsigned ccount = 1;
  static constexpr size_t subcount = 0;
  static constexpr std::basic_ostream<CharT, Traits> *(*submakers[])(
      runtime::gen &) = {};
};

template <typename Res, typename... Args>
class harness<std::function<Res(Args...)>> {
public:
  using user_class = std::function<Res(Args...)>;
  user_class *obj;
  harness(runtime::gen &g)
      : obj(new user_class([&g](Args...) { return *g.make<Res>(6); })) {}
  operator bool() const { return true; }
  using mptr = void (harness::*)();
  static constexpr unsigned mcount = 0;
  static constexpr mptr mroulette[] = {};
  static constexpr unsigned ccount = 1;
  static constexpr size_t subcount = 0;
  static constexpr user_class *(*submakers[])(runtime::gen &) = {};
};

} // namespace ramfuzz
