#include "assets/sp_menu.hpp"

#include <string>

namespace nf {

namespace {

constexpr std::size_t kItemSize = 0x18;

std::vector<MpMenuItem> load_items(const Elf32& elf, const std::string& name, std::size_t count) {
    const auto sym = elf.symbol(name);
    if (!sym) throw FormatError("ACTION.ELF has no " + name + " symbol");
    if (sym->size != kItemSize * count)
        throw FormatError(name + " is " + std::to_string(sym->size) + " bytes, expected " + std::to_string(kItemSize * count));
    const Bytes t = elf.at(sym->value, sym->size);
    std::vector<MpMenuItem> items;
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t o = i * kItemSize;
        MpMenuItem item;
        item.sprite = load<std::uint32_t>(t, o);
        item.name = load<std::uint32_t>(t, o + 4);
        item.description = load<std::uint32_t>(t, o + 8);
        item.value = load<std::uint32_t>(t, o + 12);
        const std::uint32_t enabled = load<std::uint32_t>(t, o + 16);
        if (enabled > 1) throw FormatError(name + ": menu item enable flag is not a bool");
        item.enabled = enabled != 0;
        item.disabled_label = load<std::uint32_t>(t, o + 20);
        items.push_back(item);
    }
    return items;
}

}  // namespace

SpMenuData load_sp_menu(const Elf32& elf) {
    return {load_items(elf, "sp_level", 12), load_items(elf, "difficulty", 3), load_items(elf, "cn_options", 7),
            load_items(elf, "ds_options", 4), load_items(elf, "ds_gadgets", 14), load_items(elf, "ds_weapons", 27)};
}

}  // namespace nf
