// The Volcano module: the client's internals, moving from headers to C++20
// modules one piece at a time. Today it has one partition, Application.
//
// Other files write `import Volcano;`. (A partition such as Volcano:Application
// can only be imported from inside this module, so it is re-exported here.)
export module Volcano;

export import :Application;
