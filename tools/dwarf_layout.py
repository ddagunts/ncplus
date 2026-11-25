#!/usr/bin/env python3
"""Dump struct/union/enum layouts from an ELF's DWARF, in a stable text form for diffing.

Used to make a rebuilt PowerVR DDK kernel module match LG's pvrsrvkm.ko field for field:
  PYTHONPATH=third_party/dl/pyelftools/pyelftools-0.33-py3-none-any.whl \
      python3 tools/dwarf_layout.py a.ko > a.txt
Only types whose name (or typedef name, for anonymous ones) matches the optional regex are
printed. Each type is printed once, even if several compile units define it; a type that is
defined differently in two units is printed under both layouts, marked "#2".
"""
import re
import sys

from elftools.elf.elffile import ELFFile


def tname(die, depth=0):
    """A short, comparable name for a type DIE."""
    if die is None:
        return "void"
    if depth > 8:
        return "..."
    tag = die.tag
    name = die.attributes.get("DW_AT_name")
    name = name.value.decode() if name else None
    sub = die.get_DIE_from_attribute("DW_AT_type") if "DW_AT_type" in die.attributes else None
    if tag == "DW_TAG_pointer_type":
        return tname(sub, depth + 1) + "*"
    if tag in ("DW_TAG_const_type", "DW_TAG_volatile_type"):
        return tname(sub, depth + 1)
    if tag == "DW_TAG_array_type":
        dims = ""
        for c in die.iter_children():
            if c.tag == "DW_TAG_subrange_type":
                ub = c.attributes.get("DW_AT_upper_bound")
                dims += "[%s]" % (ub.value + 1 if ub and isinstance(ub.value, int) else "")
        return tname(sub, depth + 1) + dims
    if tag == "DW_TAG_subroutine_type":
        return "fn()"
    if tag in ("DW_TAG_structure_type", "DW_TAG_union_type", "DW_TAG_enumeration_type"):
        kw = {"DW_TAG_structure_type": "struct", "DW_TAG_union_type": "union",
              "DW_TAG_enumeration_type": "enum"}[tag]
        return "%s %s" % (kw, name or "<anon>")
    return name or tag


def size_of(die):
    s = die.attributes.get("DW_AT_byte_size")
    return s.value if s else None


def member_offset(m):
    a = m.attributes.get("DW_AT_data_member_location")
    if a is None:
        return 0
    if isinstance(a.value, int):
        return a.value
    # gcc 4.x emits a location expression: DW_OP_plus_uconst <n>
    v = a.value
    if v and v[0] == 0x23:
        n, shift = 0, 0
        for b in v[1:]:
            n |= (b & 0x7F) << shift
            shift += 7
            if not b & 0x80:
                break
        return n
    return "?"


def layout(die):
    lines = []
    if die.tag == "DW_TAG_enumeration_type":
        for c in die.iter_children():
            if c.tag == "DW_TAG_enumerator":
                lines.append("  %s = %d" % (c.attributes["DW_AT_name"].value.decode(),
                                            c.attributes["DW_AT_const_value"].value))
        return lines
    for m in die.iter_children():
        if m.tag != "DW_TAG_member":
            continue
        n = m.attributes.get("DW_AT_name")
        n = n.value.decode() if n else "<anon>"
        t = m.get_DIE_from_attribute("DW_AT_type")
        bits = ""
        if "DW_AT_bit_size" in m.attributes:
            bits = " :%d@%d" % (m.attributes["DW_AT_bit_size"].value,
                                m.attributes.get("DW_AT_bit_offset").value
                                if "DW_AT_bit_offset" in m.attributes else 0)
        lines.append("  +%-5s %-28s %s%s" % (member_offset(m), n, tname(t), bits))
        # anonymous nested aggregates: expand inline so differences show up
        while t is not None and t.tag in ("DW_TAG_const_type", "DW_TAG_volatile_type"):
            t = t.get_DIE_from_attribute("DW_AT_type") if "DW_AT_type" in t.attributes else None
        if t is not None and t.tag in ("DW_TAG_structure_type", "DW_TAG_union_type") \
                and "DW_AT_name" not in t.attributes:
            lines += ["    " + l for l in layout(t)]
    return lines


def main():
    path = sys.argv[1]
    pat = re.compile(sys.argv[2]) if len(sys.argv) > 2 else None
    seen = {}
    with open(path, "rb") as f:
        elf = ELFFile(f)
        dw = elf.get_dwarf_info()
        for cu in dw.iter_CUs():
            for die in cu.iter_DIEs():
                tag = die.tag
                if tag == "DW_TAG_typedef":
                    t = die.get_DIE_from_attribute("DW_AT_type") if "DW_AT_type" in die.attributes else None
                    if t is None or t.tag not in ("DW_TAG_structure_type", "DW_TAG_union_type",
                                                  "DW_TAG_enumeration_type") \
                            or "DW_AT_name" in t.attributes:
                        continue
                    key = "typedef " + die.attributes["DW_AT_name"].value.decode()
                    body = t
                elif tag in ("DW_TAG_structure_type", "DW_TAG_union_type", "DW_TAG_enumeration_type"):
                    if "DW_AT_name" not in die.attributes or "DW_AT_declaration" in die.attributes:
                        continue
                    key = tname(die)
                    body = die
                else:
                    continue
                if pat and not pat.search(key):
                    continue
                text = "\n".join(["%s size %s" % (key, size_of(body))] + layout(body))
                variants = seen.setdefault(key, [])
                if text not in variants:
                    variants.append(text)
    for key in sorted(seen):
        for i, text in enumerate(seen[key]):
            if i:
                text = text.replace(key, key + " #%d" % (i + 1), 1)
            print(text)
            print()


if __name__ == "__main__":
    main()
