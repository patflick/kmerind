/**
 * @file    task.hpp
 * @ingroup
 * @author  tpan
 * @brief
 * @details
 *
 * Copyright (c) 2014 Georgia Institute of Technology.  All Rights Reserved.
 *
 * TODO add License
 */
#ifndef TASK_HPP_
#define TASK_HPP_

#include "taskrunner/runnable.hpp"

namespace bliss
{
namespace concurrent
{

/**
 * @class     bliss::concurrent::Task
 * @brief
 * @details   assumption is that the run method contains a loop, which
 *            calls src repeatedly to get data block, compute, then put
 *            data in Dest.  compute should take a datablock as input, and
 *            dest as output target (write directly into dest).  Src should
 *            know how to partition itself, and have a "getNextChunk"
 *            method Dest should know how to hash the values it receives,
 *            and have a "put" method.
 *
   *            because of the repeated runs, the three classes should avoid
   *            dynamic polymorphism.  we use static polymorphism to ensure
   *            that the types specified are correct.  we also use
   *            static_assert to ensure that the input/output types are
   *            matched up.
   *
   */
//  template<typename Compute, typename Input, typename Output>
  class Task : public Runnable
  {
//    protected:
//      Compute& comp;
//      Input& input;
//      Output& output;

    public:
//      Task(Compute &_comp, Input &_input, Output &_output)
//        : comp(_comp), input(_input), output(_output) {};
      Task() {};

      virtual ~Task() {};

      virtual void operator()() = 0;
  };

} /* namespace concurrent */
} /* namespace bliss */

#endif /* TASK_HPP_ */