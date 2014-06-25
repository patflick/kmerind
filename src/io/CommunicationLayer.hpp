/**
 * @file    CommunicationLayer.hpp
 * @ingroup index
 * @author  Patrick Flick <patrick.flick@gmail.com>
 * @author  Tony Pan <tpan@gatech.edu>
 * @brief
 *
 * Copyright (c) TODO
 *
 * TODO add Licence
 *
 *
 * TODO:  stopping the communication.
 *    need 1. mapping between message types.  assumption, each incoming message type corresponds to one or more outgoing message types.
 *          2. method to mark a message type producer as being done. - single thread.
 *              also notify remote receiver with done (for that message type/tag)
 *          3. receiver decrements sender count down to 0, at which point, mark the corresponding response send message as done.
 *          4. when all send types are done, and all receive types are done, the comm thread terminates.
 *          5. when all receive type are done, the recv thread terminates.
 *
 */

#ifndef BLISS_COMMUNICATION_LAYER_HPP
#define BLISS_COMMUNICATION_LAYER_HPP

#include <mpi.h>
//#include <omp.h>

// C stdlib includes
#include <assert.h>

// STL includes
#include <vector>
#include <queue>
#include <mutex>

// system includes
// TODO: make this system indepenedent!?
#include <unistd.h> // for usleep()

// BLISS includes
#include <concurrent/threadsafe_queue.hpp>
#include <concurrent/concurrent.hpp>
#include <io/message_buffers.hpp>

typedef bliss::io::MessageBuffers<bliss::concurrent::THREAD_SAFE> BuffersType;


struct ReceivedMessage
{
  uint8_t* data;
  std::size_t count;
  int tag;
  int src;

  ReceivedMessage(uint8_t* data, std::size_t count, int tag, int src)
    : data(data), count(count), tag(tag), src(src) {}
  ReceivedMessage() = delete;
  ReceivedMessage(ReceivedMessage&& other) = default;
  ReceivedMessage(const ReceivedMessage& other) = delete;
  ReceivedMessage& operator=(ReceivedMessage&& other) = default;
  ReceivedMessage& operator=(const ReceivedMessage& other) = delete;

};

// TODO: rename (either this or the ReceivedMessage) for identical naming scheme
struct SendQueueElement
{
  typename BuffersType::BufferIdType bufferId;
  int tag;
  int dst;

  SendQueueElement(BuffersType::BufferIdType _id, int _tag, int _dst)
    : bufferId(_id), tag(_tag), dst(_dst) {}
  SendQueueElement() = delete;
  SendQueueElement(SendQueueElement&& other) = default;
  SendQueueElement(const SendQueueElement& other) = delete;
  SendQueueElement& operator=(SendQueueElement&& other) = default;
  SendQueueElement& operator=(const SendQueueElement& other) = delete;
};


class CommunicationLayer
{
public:

  constexpr static int default_tag = 0;

protected:
  // request, data pointer, data size
  std::queue<std::pair<MPI_Request, ReceivedMessage> > recvInProgress;
  // request, data pointer, tag
  std::queue<std::pair<MPI_Request, SendQueueElement> > sendInProgress;

  // Outbound message structure, Multiple-Producer-Single-Consumer queue
  // consumed by the internal MPI-comm thread
  bliss::concurrent::ThreadSafeQueue<SendQueueElement> sendQueue;

  // Inbound message structure, Single-Producer-Multiple-Consumer queue
  // produced by the internal MPI-comm thread
  bliss::concurrent::ThreadSafeQueue<ReceivedMessage> recvQueue;

  // outbound temporary data buffer type for the producer threads.  ThreadSafe version for now.
  typedef BuffersType BuffersType;
  // outbound temporary data buffers.  one BuffersType per tag value.
  std::unordered_map<int, BuffersType> buffers;

  std::unordered_set<int> sendAccept;      // tags
  std::unordered_map<int, int> recvRemaining;   // tag to number of mpi processes


  typedef typename BuffersType::BufferIdType BufferIdType;

  mutable std::mutex mutex;

public:

  CommunicationLayer (const MPI_Comm& communicator, const int comm_size)
    : sendQueue(2 * omp_get_num_threads()), recvQueue(2 * comm_size),
      comm(communicator)
  {
    // init communicator rank and size
    MPI_Comm_size(comm, &commSize);
    assert(comm_size == commSize);
    MPI_Comm_rank(comm, &commRank);
  }

  virtual ~CommunicationLayer ();



  // adding the callback function with signature:
  // void(uint8_t* msg, std::size_t count, int fromRank)
  void addReceiveCallback(int tag, std::function<void(uint8_t*, std::size_t, int)> callbackFunction)
  {
    if (callbackFunctions.find(tag) != callbackFunctions.end()) {
      printf("function already registered for tag %d\n", tag);
      return;
    }

    if (recvRemaining[tag] == 0) {
      printf("function already had finished processing for tag %d.\n", tag);
      return;
    }


    if (callbackFunctions.empty())
    {
      // this is the first registered callback, thus spawn the callback
      // executer thread
      // TODO
    }
    // add the callback function to a lookup table
    callbackFunctions[tag] = callbackFunction;

    // also set the number of potential senders
    recvRemaining[tag] = commSize;
    sendAccept.insert(tag);
  }



  /**
   * sends a single message.  the message is buffered and batched before send.
   * may be called by multiple threads concurrently.
   *
   * @param data
   * @param count
   * @param dst_rank
   * @param tag
   */
  void sendMessage(const void* data, std::size_t count, int dst_rank, int tag=default_tag)
  {
    // check to see if the target tag is still accepting
    if (sendAccept.find(tag) != sendAccept.end()) {
      // TODO:  change to using FATAL on logger.
      printf("ERROR: calling CommunicationLayer::sendMessage with a tag that has been flushed already.  tag=%d\n", tag);
      return;
    }

    /// if there isn't already a tag listed, add the MessageBuffers for that tag.
    // multiple threads may call this.
    std::unique_lock<std::mutex> lock(mutex);
    if (buffers.find(tag) == buffers.end()) {
      buffers[tag] = std::move(BuffersType(commSize, 8192));
    }
    lock.unlock();

    /// try to append the new data - repeat until successful.
    /// along the way, if a full buffer's id is returned, queue it for sendQueue.
    BufferIdType fullId = -1;
    while (!buffers[tag].append(data, count, dst_rank, fullId)) {
      // repeat until success;
      if (fullId != -1) {
        if (!(buffers[tag].getBackBuffer(fullId).isEmpty())) {
        // have a full buffer - put in send queue.
//        SendQueueElement v(fullId, tag, dst_rank);
//        while (!sendQueue.tryPush(std::move(v))) {
//          usleep(20);
//        }
          sendQueue.waitAndPush(std::move(SendQueueElement(fullId, tag, dst_rank)));
        }
      }

      usleep(20);
    }

  }

  /**
   * flushes the message buffers asscoiated with a particular tag.  should be called by a single thread only.
   * @param tag
   */
  void flush(int tag)
  {

    // no MessageBuffers with the associated tag.  end.
    if (buffers.find(tag) == buffers.end())  return;

    if (sendAccept.find(tag) == sendAccept.end()) {
      // already flushed.
      return;
    }


    // flush out all the send buffers matching a particular tag.
    int i = 0;
    for (auto id : buffers[tag].getActiveIds()) {
      if ((id != -1) && !(buffers[tag].getBackBuffer(id).isEmpty())) {
//        SendQueueElement v(id, tag, i);
//        while (!sendQueue.tryPush(std::move(v))) {
//          usleep(20);
//        }
				sendQueue.waitAndPush(std::move(SendQueueElement(id, tag, i)));
      }
      // send the end message for this tag.
      sendQueue.waitAndPush(std::move(SendQueueElement(-1, tag, i)));
      ++i;
    }
    
    /// mark as no more coming in for this tag.
    sendAccept.erase(tag);
    /// leave the MessageBuffers. - have to wait until all's sent.
  }




  int getCommSize() const
  {
    return commSize;
  }

  int getCommRank() const
  {
    return commRank;
  }


      /*
      // active receiving (by polling) for when there is no callback set
      // these must be thread-safe!
      Message receiveAnyOne();
      std::vector<message> receiveAnyAll();

      Message receiveOne(tag);
      std::vector<message> receiveAll(tag);
    */

  protected:


    //
    void commThread()
    {
      // TODO: implement termination condition
      while ((sendAccept.size() > 0) || (sendQueue.size() > 0) || (sendInProgress.size() > 0)
              || (recvRemaining.size() > 0) || (recvInProgress.size() > 0) || (recvQueue.size() > 0))
      {
        // first clean up finished operations
        finishReceives();
        finishSends();

        // start pending receives
        tryStartReceive();
        // start pending sends
        tryStartSend();
      }

    }

    void callbackThread()
    {
      // TODO: add termination condition
      while ((recvRemaining.size() > 0) || (recvInProgress.size() > 0) || (recvQueue.size() > 0))
      {
        // get next element from the queue, wait if none is available
        ReceivedMessage msg;
        recvQueue.waitAndPop(msg);

        // TODO: check if the tag exists as callback function

        // call the matching callback function
        (callbackFunctions[msg.tag])(msg.data, msg.count, msg.src);
        delete [] msg.data;
      }
    }


    //// Order of message matters so make sure end message will be sent/recv'd after data messages.
    //// same source/destination messages goes between queues directly, bypassing MPI, so need to enqueue local termination message into "inprogress" (data can go into recv queue)


    void tryStartSend()
    {
      // try to get the send element
      SendQueueElement se;
      if (sendQueue.tryPop(se)) {

        if (se.bufferId == -1) {
          // termination message for this tag and destination


          if (se.dst == commRank) {
            // local, directly handle by creating an output object and directly
            // insert into the recvInProgress (need to maintain message ordering.
            recvQueue.waitAndPush(std::move(ReceivedMessage(nullptr, 0, se.tag, commRank)));


          } else {
            // remote.  send a terminating message. with the same tag to ensure message ordering.
            MPI_Request req;
            MPI_Isend(nullptr, 0, MPI_UINT8_T, se.dst, se.tag, comm, &req);
            sendInProgress.push(std::move(std::pair<MPI_Request, SendQueueElement>(req, se)));
          }

        } else {  // real data.
          auto data = buffers[se.tag].getBackBuffer(se.bufferId).getData();
          auto count = buffers[se.tag].getBackBuffer(se.bufferId).getSize();


          if (se.dst == commRank) {
            // local, directly handle by creating an output object and directly
            // insert into the recv queue
            uint8_t* array = new uint8_t[count];
            memcpy(array, data, count);

            recvQueue.waitAndPush(std::move(ReceivedMessage(array, count, se.tag, commRank)));

            // finished inserting directly to local RecvQueue.  release the buffer
            buffers[se.tag].releaseBuffer(se.bufferId);

          } else {

            MPI_Request req;
            MPI_Isend(data, count, MPI_UINT8_T, se.dst, se.tag, comm, &req);
            sendInProgress.push(std::move(std::pair<MPI_Request, SendQueueElement>(req, se)));
          }
        }
      }
    }

    void finishSends()
    {
      int finished = 0;
      while(!sendInProgress.empty())
      {
        std::pair<MPI_Request, SendQueueElement>& front = sendInProgress.front();
        if (front.first == MPI_REQUEST_NULL) finished = 1;
        else
          MPI_Test(&front.first, &finished, MPI_STATUS_IGNORE);

        if (finished)
        {
          if (front.second.bufferId != -1) {
            // cleanup, i.e., release the buffer back into the pool
            buffers[front.second.tag].releaseBuffer(front.second.bufferId);
          }
          sendInProgress.pop();
        }
        else
        {
          break;
        }
      }
    }

    void tryStartReceive()
    {
      /// probe for messages
      int hasMessage = 0;
      MPI_Status status;
      MPI_Iprobe(MPI_ANY_SOURCE, MPI_ANY_TAG, comm, &hasMessage, &status);

      if (hasMessage > 0) {
        int src = status.MPI_SOURCE;
        int tag = status.MPI_TAG;
        int received_count;
        MPI_Get_count(&status, MPI_UNSIGNED_CHAR, &received_count);

        // receive whether it's empty or not.  use finishReceive to handle the termination message and count decrement

        // receive the message data bytes into a vector of bytes
        uint8_t* msg_data = (received_count == 0) ? nullptr : new uint8_t[received_count];
        MPI_Request req;
        MPI_Irecv(msg_data, received_count, MPI_UNSIGNED_CHAR, src, tag, comm, &req);

        // insert into the in-progress queue
        recvInProgress.push(std::move(std::pair<MPI_Request,
                                                ReceivedMessage>(req,
                                                                 ReceivedMessage(msg_data, received_count, tag, src))));

      }
    }


    // Not thread safe
    void finishReceives()
    {
      int finished = 0;
      //MPI_Status status;
      while(!recvInProgress.empty())
      {
        std::pair<MPI_Request, ReceivedMessage>& front = recvInProgress.front();

        if (front.first == MPI_REQUEST_NULL) finished = 1;
        else
          MPI_Test(&front.first, &finished, MPI_STATUS_IGNORE);

        if (finished)
        {

          if (front.second.count == 0) {  // terminating message.
            // end of messaging.
            --recvRemaining[front.second.tag];
            printf("RECV rank %d receiving END signal %d from %d, num sanders remaining is %d\n", commRank, front.second.tag, front.second.src, recvRemaining[front.second.tag]);

            if (recvRemaining[front.second.tag] == 0) {
              // received all end messages.  there may still be message in progress and in recvQueue from this and other sources.
              recvRemaining.erase(front.second.tag);

              recvQueue.waitAndPush(std::move(front.second));
            } else if (recvRemaining[front.second.tag] == 0) {
              printf("ERROR: number of remaining receivers for tag %d is now NEGATIVE\n", front.second.tag);
            }

          } else {

          // add the received messages into the recvQueue
    //        ReceivedMessage msg(std::get<1>(front), std::get<2>(front),
    //                            status.MPI_TAG, status.MPI_SOURCE);
          // TODO: (maybe) one queue per tag?? Where does this
          //       sorting/categorizing happen?
//          while (!recvQueue.tryPush(std::move(front.second))) {
//            usleep(50);
//          }
            recvQueue.waitAndPush(std::move(front.second));
          }
          // remove moved element
          recvInProgress.pop();
        }
        else
        {
          // stop receiving for now
          break;
        }
      }
    }


private:
  /* data */

  /// The MPI Communicator object for this communication layer
  MPI_Comm comm;

  /// Registry of callback functions, mapped to by the associated tags
  std::map<int,std::function<void(uint8_t*, std::size_t, int)> > callbackFunctions;

  /// The MPI Communicator size
  int commSize;

  /// The MPI Communicator rank
  int commRank;
};

#endif // BLISS_COMMUNICATION_LAYER_HPP