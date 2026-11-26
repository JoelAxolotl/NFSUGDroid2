#pragma once
#include <cstdint>
#include <string>

namespace loader {

struct Image {
    uint32_t base = 0;
    uint32_t size = 0;
    uint32_t entry = 0;
    uint32_t stack_reserve = 0;
    uint32_t rsrc_rva = 0;
    uint32_t rsrc_size = 0;
    uint32_t text_start = 0, text_end = 0;
    uint32_t imports_bound = 0, imports_missing = 0;
};

// Maps an x86 PE executable at its preferred base and binds imports to HLE
// thunks. The executable has no relocations, so the preferred base is required.
bool load_exe(const std::string& host_path, Image& out, std::string& error);

const Image& main_image();

// Resource lookup on the main image (FindResourceA / LoadResource).
// `name`/`type` are either guest string pointers or MAKEINTRESOURCE ids (< 0x10000).
uint32_t find_resource(uint32_t name, uint32_t type);  // returns guest address of the data entry
uint32_t resource_data(uint32_t entry);                // guest address of the data
uint32_t resource_size(uint32_t entry);

}  // namespace loader
