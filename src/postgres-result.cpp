/**
 * Copyright (c) 2016 Philippe FERDINAND
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
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 **/

#include "postgres-connection.h"
#include "postgres-exceptions.h"

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>

#include "libpq/pg_type.h"

#ifdef __linux__
  #include <endian.h>
#elif defined (__APPLE__)
  #include <libkern/OSByteOrder.h>
  #define be16toh(x) OSSwapBigToHostInt16(x)
  #define be32toh(x) OSSwapBigToHostInt32(x)
  #define be64toh(x) OSSwapBigToHostInt64(x)
#elif defined (_WINDOWS)
  #include <winsock2.h>
  #define be16toh(x) ntohs(x)
  #define be32toh(x) ntohl(x)
  #define be64toh(x) ntohll(x)
#endif

namespace db {
  namespace postgres {

#ifdef NDEBUG

    #define assert_oid(expected, actual) ((void)0)

#else

    void assert_oid(int expected, int actual) {
      const char *_expected = nullptr;
      if (expected != actual) {
        switch (expected) {
          case BOOLOID: _expected = "std::vector<uint_8>"; break;
          case BYTEAOID: _expected = "char"; break;
          case CHAROID: _expected = "std::string"; break;
          case NAMEOID: _expected = "std::string"; break;
          case INT8OID: _expected = "int64_t"; break;
          case INT2OID: _expected = "int16_t"; break;
          case INT4OID: _expected = "int32_t"; break;
          case TEXTOID: _expected = "std::string"; break;
          case FLOAT4OID: _expected = "float"; break;
          case FLOAT8OID: _expected = "double"; break;
          case BPCHAROID: _expected = "std::string"; break;
          case VARCHAROID: _expected = "std::string"; break;
          case DATEOID: _expected = "db::postgres::date_t"; break;
          case TIMEOID: _expected = "db::postgres::time_t"; break;
          case TIMESTAMPOID: _expected = "db::postgres::timestamp_t"; break;
          case TIMESTAMPTZOID: _expected = "db::postgres::timestamptz_t"; break;
          case INTERVALOID: _expected = "db::postgres::interval_t"; break;
          case TIMETZOID: _expected = "db::postgres::timetz_t"; break;
          default:
            assert(false); // unsupported type. try std::string
        }

        std::cerr << "Unexpected C++ type. Please use get<" << _expected
          << ">(int column)" << std::endl;
        assert(false);
      }
    }
#endif

    // -------------------------------------------------------------------------
    // Reading a value from a postgresql value buffer
    // -------------------------------------------------------------------------
    template <typename T>
    T read(char **buf);

    // -------------------------------------------------------------------------
    // bool
    // -------------------------------------------------------------------------
    template <>
    bool read<bool>(char **buf) {
      auto v = *reinterpret_cast<bool*>(*buf);
      *buf += sizeof(bool);
      return v;
    }

    // -------------------------------------------------------------------------
    // smallint
    // -------------------------------------------------------------------------
    template <>
    int16_t read<int16_t>(char **buf) {
      auto v = be16toh(*reinterpret_cast<int16_t*>(*buf));
      *buf += sizeof(int16_t);
      return v;
    }

    // -------------------------------------------------------------------------
    // integer
    // -------------------------------------------------------------------------
    template <>
    int32_t read<int32_t>(char **buf) {
      auto v = be32toh(*reinterpret_cast<int32_t*>(*buf));
      *buf += sizeof(int32_t);
      return v;
    }

    // -------------------------------------------------------------------------
    // bigint
    // -------------------------------------------------------------------------
    template <>
    int64_t read<int64_t>(char **buf) {
      auto v = be64toh(*reinterpret_cast<int64_t*>(*buf));
      *buf += sizeof(int64_t);
      return v;
    }

    // -------------------------------------------------------------------------
    // real
    // -------------------------------------------------------------------------
    template <>
    float read<float>(char **buf) {
      int32_t v = read<int32_t>(buf);
      return *reinterpret_cast<float*>(&v);
    }

    // -------------------------------------------------------------------------
    // double precision
    // -------------------------------------------------------------------------
    template <>
    double read<double>(char **buf) {
      int64_t v = read<int64_t>(buf);
      return *reinterpret_cast<double*>(&v);
    }

    // -------------------------------------------------------------------------
    // date
    // -------------------------------------------------------------------------
    template <>
    date_t read<date_t>(char **buf) {
      return date_t {
        (read<int32_t>(buf) + DAYS_UNIX_TO_J2000_EPOCH) * 86400
      };
    }

    // -------------------------------------------------------------------------
    // timestamp
    // -------------------------------------------------------------------------
    template <>
    timestamp_t read<timestamp_t>(char **buf) {
      return timestamp_t {
        read<int64_t>(buf) + MICROSEC_UNIX_TO_J2000_EPOCH
      };
    }

    // -------------------------------------------------------------------------
    // timestamptz
    // -------------------------------------------------------------------------
    template <>
    timestamptz_t read<timestamptz_t>(char **buf) {
      return timestamptz_t {
        read<int64_t>(buf) + MICROSEC_UNIX_TO_J2000_EPOCH
      };
    }

    // -------------------------------------------------------------------------
    // timetz
    // -------------------------------------------------------------------------
    template <>
    timetz_t read<timetz_t>(char **buf) {
      return timetz_t {
        read<int64_t>(buf),
        read<int32_t>(buf)
      };
    }

    // -------------------------------------------------------------------------
    // time
    // -------------------------------------------------------------------------
    template <>
    time_t read<time_t>(char **buf) {
      return time_t { read<int64_t>(buf) };
    }

    // -------------------------------------------------------------------------
    // interval
    // -------------------------------------------------------------------------
    template <>
    interval_t read<interval_t>(char **buf) {
      return interval_t {
        read<int64_t>(buf),
        read<int32_t>(buf),
        read<int32_t>(buf)
      };
    }

    // -------------------------------------------------------------------------
    // Reading a value from a PGresult
    // -------------------------------------------------------------------------
    template <typename T>
    T read(const PGresult *pgresult, int column) {
      char *buf = PQgetvalue(pgresult, 0, column);
      return read<T>(&buf);
    }

    template <typename T>
    T read(const PGresult *pgresult, int oid, int column, T defVal) {
      assert(pgresult != nullptr);
      assert_oid(PQftype(pgresult, column), oid);
      return PQgetisnull(pgresult, 0, column) ? defVal : read<T>(pgresult, column);
    }

    template<typename T>
    std::vector<T> readArray(const PGresult *pgresult, int oid, int column, T defVal) {
      std::vector<T> array;

      if (!PQgetisnull(pgresult, 0, column)) {
        // The data should look like this:
        //
        // struct pg_array {
        //   int32_t ndim; /* Number of dimensions */
        //   int32_t ign;  /* offset for data, removed by libpq */
        //   Oid elemtype; /* type of element in the array */
        //
        //   /* First dimension */
        //   int32_t size;  /* Number of elements */
        //   int32_t index; /* Index of first element */
        //   T first_value; /* Beginning of the data */
        // }
        char *buf = PQgetvalue(pgresult, 0, column);
        int32_t ndim = read<int32_t>(&buf);
        int32_t ign = read<int32_t>(&buf);
        int32_t elemType = read<int32_t>(&buf);
        assert_oid(elemType, oid);
        assert(ndim == 1); // only array of 1 dimmension are supported so far.

        // First dimension
        int32_t size = read<int32_t>(&buf);
        int32_t index = read<int32_t>(&buf);

        int32_t elemSize;
        array.reserve(size);
        for (int32_t i=0; i < size; i++) {
          elemSize = read<int32_t>(&buf);
          array.push_back(elemSize == -1 ? defVal : read<T>(&buf));
        }
      }

      return array;
    }

    // -------------------------------------------------------------------------
    // Row contructor
    // -------------------------------------------------------------------------
    Row::Row(Result &result)
    : result_(result) {
    }

    int Row::num() const noexcept {
      return result_.num_;
    }

    // -------------------------------------------------------------------------
    // Tests a column for a null value.
    // -------------------------------------------------------------------------
    bool Row::isNull(int column) const {
      assert(result_.pgresult_ != nullptr);
      return PQgetisnull(result_, 0, column) == 1;
    }

    // -------------------------------------------------------------------------
    // Get a column name.
    // -------------------------------------------------------------------------
    const char *Row::columnName(int column) const {
      assert(result_.pgresult_ != nullptr);
      const char *res = PQfname(result_, column);
      assert(res);
      return res;
    }

    template<>
    bool Row::get<bool>(int column) const {
      return read<bool>(result_, BOOLOID, column, false);
    }

    template<>
    int16_t Row::get<int16_t>(int column) const {
      return read<int16_t>(result_, INT2OID, column, 0);
    }

    template<>
    int32_t Row::get<int32_t>(int column) const {
      return read<int32_t>(result_, INT4OID, column, 0);
    }

    template<>
    int64_t Row::get<int64_t>(int column) const {
      return read<int64_t>(result_, INT8OID, column, 0);
    }

    template<>
    float Row::get<float>(int column) const {
      return read<float>(result_, FLOAT4OID, column, 0);
    }

    template<>
    double Row::get<double>(int column) const {
      return read<double>(result_, FLOAT8OID, column, 0);
    }

    // -------------------------------------------------------------------------
    // char, varchar, text
    // -------------------------------------------------------------------------
    template<>
    std::string Row::get<std::string>(int column) const {
      assert(result_.pgresult_ != nullptr);
      if (PQgetisnull(result_, 0, column)) {
        return std::string();
      }
      int length = PQgetlength(result_, 0, column);
      return std::string(PQgetvalue(result_, 0, column), length);
    }

    // -------------------------------------------------------------------------
    // "char"
    // -------------------------------------------------------------------------
    template<>
    char Row::get<char>(int column) const {
      assert(result_.pgresult_ != nullptr);
      if (PQgetisnull(result_, 0, column)) {
        return '\0';
      }
      assert(PQgetlength(result_, 0, column) == 1);
      return *PQgetvalue(result_, 0, column);
    }

    // -------------------------------------------------------------------------
    // bytea
    // -------------------------------------------------------------------------
    template<>
    std::vector<uint8_t> Row::get<std::vector<uint8_t>>(int column) const {
      assert(result_.pgresult_ != nullptr);
      assert_oid(PQftype(result_, column), BYTEAOID);
      int length = PQgetlength(result_, 0, column);
      uint8_t *data = reinterpret_cast<uint8_t *>(PQgetvalue(result_, 0, column));
      return std::vector<uint8_t>(data, data + length);
    }

    template<>
    date_t Row::get<date_t>(int column) const {
      return read<date_t>(result_, DATEOID, column, date_t { 0 });
    }

    template<>
    timestamptz_t Row::get<timestamptz_t>(int column) const {
      return read<timestamptz_t>(result_, TIMESTAMPTZOID, column, timestamptz_t { 0 });
    }

    template<>
    timestamp_t Row::get<timestamp_t>(int column) const {
      return read<timestamp_t>(result_, TIMESTAMPOID, column, timestamp_t { 0 });
    }

    template<>
    timetz_t Row::get<timetz_t>(int column) const {
      return read<timetz_t>(result_, TIMETZOID, column, timetz_t { 0, 0 });
    }

    template<>
    time_t Row::get<time_t>(int column) const {
      return read<time_t>(result_, TIMEOID, column, time_t { 0 });
    }

    template<>
    interval_t Row::get<interval_t>(int column) const {
      return read<interval_t>(result_, INTERVALOID, column, interval_t { 0, 0, 0 });
    }

    template<>
    std::vector<bool> Row::asArray<bool>(int column) const {
      return readArray<bool>(result_, BOOLOID, column, 0);
    }

    template<>
    std::vector<int16_t> Row::asArray<int16_t>(int column) const {
      return readArray<int16_t>(result_, INT2OID, column, 0);
    }

    template<>
    std::vector<int32_t> Row::asArray<int32_t>(int column) const {
      return readArray<int32_t>(result_, INT4OID, column, 0);
    }

    // -------------------------------------------------------------------------
    // Result contructor
    // -------------------------------------------------------------------------
    Result::Result(Connection &conn)
      : Row(*this), conn_(conn), begin_(*this), end_(*this) {
      pgresult_ = nullptr;
      status_ = PGRES_EMPTY_QUERY;
    }

    // -------------------------------------------------------------------------
    // Destructor
    // -------------------------------------------------------------------------
    Result::~Result() {
      if (pgresult_) {
        PQclear(pgresult_);
      }
    }

    // -------------------------------------------------------------------------
    // First row of the result
    // -------------------------------------------------------------------------
    Result::iterator Result::begin() {
      return Result::iterator(&begin_);
    }

    // -------------------------------------------------------------------------
    // Last row of the result
    // -------------------------------------------------------------------------
    Result::iterator Result::end() {
      // If there is no result available then end() = begin()
      return Result::iterator(status_ == PGRES_SINGLE_TUPLE ? &end_ : &begin_);
    }

    // -------------------------------------------------------------------------
    // Next row of the result
    // -------------------------------------------------------------------------
    Result::iterator Result::iterator::operator ++() {
      ptr_->result_.next();
      if (ptr_->result_.status_ != PGRES_SINGLE_TUPLE) {
        // We've reached the end
        assert(ptr_->result_.status_ == PGRES_TUPLES_OK);
        ptr_ = &ptr_->result_.end_;
      }
      return iterator (ptr_);
    }

    // -------------------------------------------------------------------------
    // Number of rows affected by the SQL command
    // -------------------------------------------------------------------------
    uint64_t Result::count() const noexcept {
      assert(pgresult_);
      const char *count = PQcmdTuples(pgresult_);
      char *end;
      return std::strtoull(count, &end, 10);
    }

    // -------------------------------------------------------------------------
    // Get the first result from the server.
    // -------------------------------------------------------------------------
    void Result::first() {
      assert(pgresult_ == nullptr);
      num_ = 0;
      next();
    }

    // -------------------------------------------------------------------------
    // Get the next result from the server.
    // -------------------------------------------------------------------------
    void Result::next() {

      if (pgresult_) {
        assert(status_ == PGRES_SINGLE_TUPLE);
        PQclear(pgresult_);
      }

      pgresult_ = PQgetResult(conn_);
      assert(pgresult_);
      status_ = PQresultStatus(pgresult_);
      switch (status_) {
        case PGRES_SINGLE_TUPLE:
          assert(PQntuples(pgresult_) == 1);
          num_++;
          break;

        case PGRES_TUPLES_OK:
          // The SELECT statement did not return any row or after the last row,
          // a zero-row object with status PGRES_TUPLES_OK is returned; this is
          // the signal that no more rows are expected.
          assert(PQntuples(pgresult_) == 0);
          break;

        case PGRES_BAD_RESPONSE:
        case PGRES_FATAL_ERROR:
          throw ExecutionException(conn_.lastError());
          break;

        case PGRES_COMMAND_OK:
          break;

        default:
          assert(false);
          break;
      }

    }

    // -------------------------------------------------------------------------
    // Clear the previous result of the connection
    // -------------------------------------------------------------------------
    void Result::clear() {

      switch (status_) {
        case PGRES_COMMAND_OK:
          do {
            PQclear(pgresult_);
            pgresult_ = PQgetResult(conn_);
            if (pgresult_ == nullptr) {
              status_ = PGRES_EMPTY_QUERY;
            }
            else {
              status_ = PQresultStatus(pgresult_);
              switch (status_) {
                case PGRES_COMMAND_OK:
                  break;

                case PGRES_BAD_RESPONSE:
                case PGRES_FATAL_ERROR:
                  throw ExecutionException(conn_.lastError());
                  break;

                case PGRES_SINGLE_TUPLE:
                case PGRES_TUPLES_OK:
                  // It is not supported to execute a multi statement sql
                  // without fetching the result with the result iterator.
                  assert(true);
                  break;

                default:
                  assert(true);
                  break;
              }
            }
          } while (status_ != PGRES_EMPTY_QUERY);
          break;

        case PGRES_BAD_RESPONSE:
        case PGRES_FATAL_ERROR:
        case PGRES_TUPLES_OK:
          PQclear(pgresult_);
          pgresult_ = PQgetResult(conn_);
          assert(pgresult_ == nullptr);
          status_ = PGRES_EMPTY_QUERY;
          break;

        case PGRES_SINGLE_TUPLE:
          next();
          if (status_ == PGRES_SINGLE_TUPLE) {
            // All results of the previous query have not been processed, we
            // need to cancel it.
            conn_.cancel();
          }
          else if (status_ == PGRES_TUPLES_OK) {
            PQclear(pgresult_);
            pgresult_ = PQgetResult(conn_);
            status_ = PGRES_EMPTY_QUERY;
          }
          assert(pgresult_ == nullptr);
          break;

        case PGRES_EMPTY_QUERY:
          break;

        default:
          assert(true);
      }

    }

  } // namespace postgres
}   // namespace db
