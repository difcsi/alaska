#pragma once


namespace alaska {

  // The WorkScheduler's main job is to allow work to be scheduled and executed at some
  // point in the future. When that is, exactly, is not well defined. The goal is to batch
  // work together to reduce the number of stop-the-world barrier invocations.
  class WorkScheduler {
    //
  };

}  // namespace alaska