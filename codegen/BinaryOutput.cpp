#include "BinaryOutput.h"

#define DEBUG
#include "../utils/logging.h"


Result<void> BinaryOutput::link() {
    if (spaces.empty()) LOGD("linked binary doesnt contain any spaces");

    // auto startPtr = &bytes[0];

    for (const auto& [name, space] : spaces) {
        if (!labels.contains(name)) return FAILF("label \"{}\" not found", name);

        auto tag = labels[name];

        if (tag.size > space.size) return FAILF("symbol is dataSize of {} bytes, but space is dataSize of {} bytes", tag.size, space.size);
        TODO();
   /*     if (space.isRelative) {
            unsigned char offset = static_cast<u8>((space.index + space.size) - tag.index);
            LOGDF("writing offset {:x} at index {:x}", offset, space.index);

            startPtr[space.index] = offset;
        }
        else {
            TODO();
        }*/
    }

    return {};
}
