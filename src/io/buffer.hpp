/**
 * @file		Buffer.hpp
 * @ingroup bliss::io
 * @author	tpan
 * @brief   fixed sized memory buffer declaration and implementations.
 * @details templated memory buffer classes with fixed sized byte array for storage, including thread safe and thread unsafe declarations and implementations.
 *
 *
 * Copyright (c) 2014 Georgia Institute of Technology.  All Rights Reserved.
 *
 * TODO add License
 */
#ifndef BUFFER_HPP_
#define BUFFER_HPP_

#include <cstring>   // memset
//#include <cstdlib>

#include <atomic>
#include <mutex>

#include <memory>     // unique_ptr
#include <utility>    // move, forward, swap, make_pair
#include <stdexcept>

#include "concurrent/concurrent.hpp"   // ThreadSafety boolean constants

namespace bliss
{
  namespace io
  {

    /**
     * @brief Thread safe/unsafe Memory Buffer, which is a fixed size allocated block of memory where data can be appended to, and read from via pointer.
     * @details this class uses unique_ptr internally to manage the memory, and supports only MOVE semantics.
     *
     * Thread Safety is enforced using atomic variables - writing to offsets that are atomically computed.
     * Memory ordering uses acquire/consume and release, and avoids seq_cst.
     *
     * Only when necessary, mutex lock is used. (in move constructor, move assignment operator, and in append function)
     * See append function for information about why mutex lock is needed.
     *
     * thread-safe and thread-unsafe versions can be move constructed/assigned from each other.
     * @tparam  ThreadSafety  controls whether this class is thread safe or not
     */
    template<bliss::concurrent::ThreadSafety ThreadSafety>
    class Buffer
    {
      /*
       * Declare Buffer<!ThreadSafety> class as a friend class, so we can reference its member functions and variables
       * in move constructor and assignment operators that moves data between instances with different thread safety.
       *
       * alternative is to use inheritance, which would require virtual functions that are potentially expensive.
       */
      friend class Buffer<!ThreadSafety>;

      public:
        /// type of internal data pointer.
        typedef std::unique_ptr<uint8_t[], std::default_delete<uint8_t[]> >  DataType;


      protected:

        /// maximum capacity
        size_t capacity;

        /**
         * @brief current occupied size of the buffer
         * @details depending on thread safety, either an atomic or a raw data type.
         *
         * @note mutable so a const Buffer object can still call append and modify the internal. (conceptually, size and data are constant members of the Buffer)
         */
        mutable typename std::conditional<ThreadSafety, std::atomic<size_t>, size_t>::type size;

        /**
         * @brief indicating whether the buffer is accepting data or not.
         * @details depending on thread safety, either an atomic or a raw data type.
         *          this is useful when swapping buffers in bufferPool or in MessageBuffers classes.
         *
         * @note mutable so a const Buffer object can still call append and modify the internal. (conceptually, size and data are constant members of the Buffer)
         */
        mutable typename std::conditional<ThreadSafety, std::atomic<bool>, bool>::type blocked;

        /// internal data storage
        DataType data;

        /// mutex for locking access to the buffer.  available in both thread safe and unsafe versions so we on't need to extensively enable_if or inherit
        mutable std::mutex mutex;

      private:
        /**
         * @brief Move constructor with a mutex lock on the source object.
         * @details This constructor is private and only use as constructor delegation target.
         *  the source object is locked before this function is called and data moved to the newly constructed object.
         * This version copies between 2 objects with the SAME thread safety.
         *
         * @param other   the source Buffer
         * @param l       the mutex lock on the source Buffer.
         */
        Buffer(Buffer<ThreadSafety>&& other, const std::lock_guard<std::mutex> &l) :
          capacity(other.capacity), size(other.getSize<ThreadSafety>()),
          blocked(other.isBlocked<ThreadSafety>()), data(std::move(other.data)) {
          other.size = 0;
          other.capacity = 0;
          other.blocked = true;
          other.data = nullptr;
        };

        /**
         * @brief Move constructor with a mutex lock on the move source object.
         * @details  This constructor is private and only use as constructor delegation target.
         * the source object is locked before this function is called and data moved to the newly constructed object.
         * This version copies between 2 objects with the DIFFERENT thread safety.
         *
         * @param other   the source Buffer
         * @param l       the mutex lock on the source Buffer.
         */
        Buffer(Buffer<!ThreadSafety>&& other, const std::lock_guard<std::mutex> &l);

      public:
        /**
         * @brief Normal constructor.  Allocate and initialize memory with size specified as parameter.
         * @param _capacity   The maximum capacity of the Buffer in bytes
         */
        explicit Buffer(const size_t _capacity) : capacity(_capacity), size(0), blocked(false), data(new uint8_t[_capacity])
        {
          if (_capacity == 0)
            throw std::invalid_argument("Buffer constructor parameter capacity is given as 0");

          memset(data.get(), 0, _capacity * sizeof(uint8_t));
        };

        /**
         * @brief reate a new Buffer using the memory specified along with allocated memory's size.
         * @param _data   The pointer/address of preallocated memory block
         * @param count   The size of preallocated memory block
         */
        Buffer(void* _data, const size_t count) : capacity(count), size(count), blocked(false), data(static_cast<uint8_t*>(_data)) {
          if (count == 0)
            throw std::invalid_argument("Buffer constructor parameter count is given as 0");
          if (_data == nullptr)
            throw std::invalid_argument("Buffer constructor parameter _data is given as nullptr");
        }

        /**
         * @brief Destructor.  deallocation of memory is automatic.
         */
        virtual ~Buffer() {};

        /**
         * @brief Move constructs from a Buffer with the SAME ThreadSafety property
         * @details  internal data memory moved by std::unique_ptr semantics.
         * Thread Safe always, using approach from http://www.justsoftwaresolutions.co.uk/threading/thread-safe-copy-constructors.html
         *
         * Constructs a mutex lock and then delegates to another constructor.
         *
         *
         * @param other   Source object to move
         */
        explicit Buffer(Buffer<ThreadSafety>&& other) : Buffer<ThreadSafety>(std::move(other), std::lock_guard<std::mutex>(other.mutex) ) {};

        /**
         * @brief Move constructs from a Buffer with the DIFFERENT ThreadSafety property
         * @details internal data memory moved by std::unique_ptr semantics.
         * Thread Safe always, using approach from http://www.justsoftwaresolutions.co.uk/threading/thread-safe-copy-constructors.html
         *
         * Constructs a mutex lock and then delegates to another constructor.
         *
         * @param other   Source object to move
         */
        explicit Buffer(Buffer<!ThreadSafety>&& other) : Buffer<ThreadSafety>(std::move(other), std::lock_guard<std::mutex>(other.mutex) ) {};

        /**
         * @brief Move assignment operator, between Buffers of the SAME ThreadSafety property.
         * @details  Internal data memory moved by std::unique_ptr semantics.
         * The move is done in a thread safe way always.
         *
         * @param other     Source Buffer to move
         * @return          target Buffer reference
         */
        Buffer<ThreadSafety>& operator=(Buffer<ThreadSafety>&& other) {
          if (this->data != other.data) {

            std::unique_lock<std::mutex> myLock(mutex, std::defer_lock),
                                         otherLock(other.mutex, std::defer_lock);
            std::lock(myLock, otherLock);

            /// move the internal memory.
            capacity = other.capacity;
            size = other.getSize<ThreadSafety>();
            blocked = other.isBlocked<ThreadSafety>();
            data = std::move(other.data);

            other.capacity = 0;
            other.size = 0;
            other.blocked = true;
            other.data = nullptr;
          }
          return *this;
        }

        /**
         * @brief Move assignment operator between Buffers of the DIFFERENT ThreadSafety property.
         * @details  Internal data memory moved by std::unique_ptr semantics.
         * The move is done in a thread safe way always.
         *
         * @param other     Source Buffer to move
         * @return          target Buffer reference
         */
        Buffer<ThreadSafety>& operator=(Buffer<!ThreadSafety>&& other);


        /// remove copy constructor and copy assignement operators.
        explicit Buffer(const Buffer<ThreadSafety>& other) = delete;
        /// remove copy constructor and copy assignement operators.
        Buffer<ThreadSafety>& operator=(const Buffer<ThreadSafety>& other) = delete;


        /**
         * @brief get the current size of the Buffer.
         * @tparam TS Choose thread safe vs unsafe implementation. defaults to same as the parent class.
         * @return    current size
         */
        template<bliss::concurrent::ThreadSafety TS = ThreadSafety>
        typename std::enable_if<TS, const size_t>::type getSize() const {
          return size.load(std::memory_order_consume);
        }

        /**
         * @brief get the current size of the Buffer.
         * @tparam TS Choose thread safe vs unsafe implementation. defaults to same as the parent class.
         * @return    current size
         */
        template<bliss::concurrent::ThreadSafety TS = ThreadSafety>
        typename std::enable_if<!TS, const size_t>::type getSize() const {
          return size;
        }

        /**
         * @brief get the status of whether buffer is accepting new appends
         * @tparam TS Choose thread safe vs unsafe implementation. defaults to same as the parent class.
         * @return    true if buffer is NOT accepting more data
         */
        template<bliss::concurrent::ThreadSafety TS = ThreadSafety>
        typename std::enable_if<TS, const bool>::type isBlocked() const {
          return blocked.load(std::memory_order_consume);
        }
        /**
         * @brief get the status of whether buffer is accepting new appends
         * @tparam TS Choose thread safe vs unsafe implementation. defaults to same as the parent class.
         * @return    true if buffer is NOT accepting more data
         */
        template<bliss::concurrent::ThreadSafety TS = ThreadSafety>
        typename std::enable_if<!TS, const bool>::type isBlocked() const {
          return blocked;
        }
        /**
         * @brief block the buffer from accepting more data.
         * @tparam TS Choose thread safe vs unsafe implementation. defaults to same as the parent class.
         */
        template<bliss::concurrent::ThreadSafety TS = ThreadSafety>
        typename std::enable_if<TS, void>::type block() const {
          blocked.store(true, std::memory_order_release);
        }
        /**
         * @brief block the buffer from accepting more data.
         * @tparam TS Choose thread safe vs unsafe implementation. defaults to same as the parent class.
         */
        template<bliss::concurrent::ThreadSafety TS = ThreadSafety>
        typename std::enable_if<!TS, void>::type block() const {
          blocked = true;
        }

      protected:
        /**
         * @brief unblock the buffer to accept more data.
         * @tparam TS Choose thread safe vs unsafe implementation. defaults to same as the parent class.
         */
        template<bliss::concurrent::ThreadSafety TS = ThreadSafety>
        typename std::enable_if<TS, void>::type unblock() const {
          blocked.store(false, std::memory_order_release);
        }
        /**
         * @brief unblock the buffer to accept more data.
         * @tparam TS Choose thread safe vs unsafe implementation. defaults to same as the parent class.
         */
        template<bliss::concurrent::ThreadSafety TS = ThreadSafety>
        typename std::enable_if<!TS, void>::type unblock() const {
          blocked = false;
        }

      public:
        /**
         * @brief get the capacity of the buffer.
         * @return    maximum capacity of buffer.
         */
        const size_t & getCapacity() const {
          return capacity;
        }

        /**
         * @brief Get a pointer to the buffer data memory block.
         *
         * @note the data should not be deleted by a calling function/thread.
         * The access is read only.  There is no reason to return the unique_ptr.
         *
         * const because the caller will have a const reference to the buffer
         */
        const void* getData() const {
          return data.get();
        }


        /**
         * @brief Clears the buffer. (set the size to 0, leaving the capacity and the memory allocation intact)
         * @note
         * const because the caller will have a const reference to the buffer.
         */
        void clear() const {
          size = 0UL;
          this->unblock<ThreadSafety>();
        }

        /**
         * @brief Checks if a buffer is full.
         * @note The return value is not precise - between getSize and return, other threads may have modified the size.
         *  For "isFull" error is infrequent.
         *
         * @return    true if the buffer is full, false otherwise.
         */
        const bool isFull() const {
          return getSize<ThreadSafety>() >= capacity;
        }

        /**
         * @brief Checks if a buffer is empty.
         * @note The return value is not precise - between getSize and return, other threads may have modified the size.
         * For "isEmpty" error is infrequent.
         *
         * @return    true if the buffer is empty, false otherwise.
         */
        const bool isEmpty() const {
          return getSize<ThreadSafety>() == 0;
        }


        /**
         * @brief Append data to the buffer, THREAD SAFE.
         * @details The function updates the current occupied size of the Buffer
         *  and memcopies the supplied data into the internal memory block.
         *
         *  This is the THREAD SAFE version using MUTEX LOCK.
         *
         * Semantically, a "false" return value means there is not enough room for the new data, not that the buffer is full.
         *
         * method is const because the caller will have const reference to the Buffer.
         *
         * NOTE: we can't use memory ordering alone.  WE have to lock with a mutex or use CAS in a loop
         * Suppose count can be different for each call, and there are 3 threads calling with count1, count2, and count3 in that order.
         *  further suppose s+count1 > capacity, count1 + count2 < count1, count1
         * then it could happen that
         *    thread1 fetch_add s' to s+count1 > capacity,
         *        thread 2 fetch_add s' to s + count1+count2 > capacity,
         *    thread1 fetch_sub s' to s+count2 < capacity,
         *            thread3 fetch_add s' to s+count2 + count3 < capacity,
         *        thread2 fetch_sub s' to s+count3,
         *            thread3 writes to s' at s+count2.
         * In the end thread 3 has written the data between s+count2 and s+count2+count3,  while s has been set to s+count3.
         * To avoid this situation, this set of calls has to be locked with mutex (else we need to use CAS in a loop)
         *
         *
         * @tparam TS Choose thread safe vs unsafe implementation. defaults to same as the parent class.
         * @param[in] _data   pointer to data to be copied into the Buffer
         * @param[in] count   number of bytes to be copied into the Buffer
         * @return            bool indicated whether operation succeeded.
         */
        template<bliss::concurrent::ThreadSafety TS = ThreadSafety>
        typename std::enable_if<TS, const bool>::type append(const void* _data, const size_t count) const {

          if (capacity == 0)
            throw std::length_error("Buffer capacity is 0");

          if (isBlocked<TS>()) return false;

          std::unique_lock<std::mutex> lock(mutex);
          size_t s = size.fetch_add(count, std::memory_order_relaxed);  // no memory ordering needed within mutex lock
          if (s + count > capacity) {
            size.fetch_sub(count, std::memory_order_relaxed);
            return false;
          }
          lock.unlock();

          std::memcpy(data.get() + s, _data, count);
          return true;
        }

        /**
         * @brief Append data to the buffer, THREAD UNSAFE.
         * @details  The function updates the current occupied size of the Buffer
         * and memcopies the supplied data into the internal memory block.
         *
         *  This is the THREAD UNSAFE version.
         *
         * Semantically, a "false" return value means there is not enough room for the new data, not that the buffer is full.
         *
         * method is const because the caller will have const reference to the Buffer.
         *
         * @tparam TS Choose thread safe vs unsafe implementation. defaults to same as the parent class.
         * @param[in] _data   pointer to data to be copied into the Buffer
         * @param[in] count   number of bytes to be copied into the Buffer
         * @return            bool indicated whether operation succeeded.
         */
        template<bliss::concurrent::ThreadSafety TS = ThreadSafety>
        typename std::enable_if<!TS, const bool>::type append(const void* typed_data, const size_t count) const {

          if (capacity == 0)
            throw std::length_error("Buffer capacity is 0");

          if (isBlocked<TS>()) return false;
          if ((size + count) > capacity) return false;

          std::memcpy(data.get() + size, typed_data, count);
          size += count;
          return true;
        }

        /**
         * @brief Append data to the buffer, THREAD SAFE.
         * @details  The function updates the current occupied size of the Buffer
         * and memcopies the supplied data into the internal memory block.
         *
         * This is the THREAD SAFE version using Compare And Swap operation in a loop, lookfree.
         *  sync version is faster on 4 threads.  using compare_exchange_strong (_weak causes extra loops but is supposed to be faster)
         *    using relaxed and release vs acquire and acq_rel - no major difference.
         *
         * Semantically, a "false" return value means there is not enough room for the new data, not that the buffer is full.
         *
         * method is const because the caller will have const reference to the Buffer.
         *
         * NOTE: we can't use memory ordering alone.  WE have to lock with a mutex or use CAS in a loop
         * See Thread Safe "append" method documentation for rationale.
         *
         * @tparam TS Choose thread safe vs unsafe implementation. defaults to same as the parent class.
         * @param[in] _data   pointer to data to be copied into the Buffer
         * @param[in] count   number of bytes to be copied into the Buffer
         * @return            bool indicated whether operation succeeded.
         */
        template<bliss::concurrent::ThreadSafety TS = ThreadSafety>
        typename std::enable_if<TS, const bool>::type append_lockfree(const void* typed_data, const size_t count) const {

          if (capacity == 0)
            throw std::length_error("Buffer capacity is 0");

          if (isBlocked<TS>()) return false;

          // try with compare and exchange weak.
          size_t s = size.load(std::memory_order_consume);
          size_t ns = s + count;
          if (ns > capacity) return false;
          while (!size.compare_exchange_strong(s, ns, std::memory_order_acq_rel, std::memory_order_acquire)) {
            // choosing strong, slower but less spurious "false" return, and should allow the "return" statement to more reliably be reached.
            ns = s + count;
            if (ns > capacity) return false;
          }

          std::memcpy(data.get() + s, typed_data, count);
          return true;
        }

    };

    /*
     * move constructor, unsafe buffer to safe buffer
     */
    template<>
    Buffer<bliss::concurrent::THREAD_SAFE>::Buffer(Buffer<bliss::concurrent::THREAD_UNSAFE>&& other, const std::lock_guard<std::mutex> &)
        : capacity(other.capacity), size(other.getSize()), blocked(other.isBlocked()), data(std::move(other.data)) {
      other.size = 0;
      other.block();
      other.capacity = 0;
      other.data = nullptr;
    };

    /*
     * move constructor, safe buffer to unsafe buffer
     */
    template<>
    Buffer<bliss::concurrent::THREAD_UNSAFE>::Buffer(Buffer<bliss::concurrent::THREAD_SAFE>&& other, const std::lock_guard<std::mutex> &)
        : capacity(other.capacity), size(other.getSize()), blocked(other.isBlocked()), data(std::move(other.data)) {
      other.size = 0;
      other.capacity = 0;
      other.block();
      other.data = nullptr;
    };

    /*
     * move constructor, unsafe buffer to safe buffer
     */
    template<>
    Buffer<bliss::concurrent::THREAD_SAFE>& Buffer<bliss::concurrent::THREAD_SAFE>::operator=(Buffer<bliss::concurrent::THREAD_UNSAFE>&& other) {
      if (this->data != other.data) {

        std::unique_lock<std::mutex> myLock(mutex, std::defer_lock),
                                     otherLock(other.mutex, std::defer_lock);
        std::lock(myLock, otherLock);

        /// move the internal memory.
        capacity = other.capacity;
        size = other.getSize();
        blocked = other.isBlocked();
        data = std::move(other.data);

        other.capacity = 0;
        other.size = 0;
        other.block();
        other.data = nullptr;
      }
      return *this;
    }

    /*
     * move constructor, safe buffer to unsafe buffer
     */
    template<>
    Buffer<bliss::concurrent::THREAD_UNSAFE>& Buffer<bliss::concurrent::THREAD_UNSAFE>::operator=(Buffer<bliss::concurrent::THREAD_SAFE>&& other) {
      if (this->data != other.data) {

        std::unique_lock<std::mutex> myLock(mutex, std::defer_lock),
                                     otherLock(other.mutex, std::defer_lock);
        std::lock(myLock, otherLock);

        /// move the internal memory.
        capacity = other.capacity;
        size = other.getSize();
        blocked = other.isBlocked();
        data = std::move(other.data);

        other.capacity = 0;
        other.size = 0;
        other.block();
        other.data = nullptr;
      }
      return *this;
    }

  } /* namespace io */
} /* namespace bliss */

#endif /* BUFFERPOOL_HPP_ */
