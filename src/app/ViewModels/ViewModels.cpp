#include "pch.h"

#include "ViewModels.h"

// View models are fully header-implemented; this TU exists so the IDL-generated
// factory registrations get compiled and linked.
namespace winrt::DiskForge::VMs::factory_implementation {
static_assert(sizeof(int) > 0, "VM factories live in headers");
}
