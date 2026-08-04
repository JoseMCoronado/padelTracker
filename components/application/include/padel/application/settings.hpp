#pragma once

#include <vector>

#include "padel/common/ids.hpp"

namespace padel::application {

// Persistent court settings (spec 10.8: the allow-list must survive
// reboots). NVS-backed on the device, file/fake natively.
struct StoredAssignment {
    RemoteId remote_id = 0;
    TeamId team{TeamId::A};
};

class ISettings {
public:
    virtual ~ISettings() = default;
    virtual std::vector<StoredAssignment> load_assignments() = 0;
    virtual bool save_assignments(const std::vector<StoredAssignment>& assignments) = 0;
};

}  // namespace padel::application
