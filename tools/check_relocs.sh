#!/bin/sh
# Guards the one rule the flat shellcode link imposes on C source:
# no initialised pointer data.
#
# linker.ld discards .rela.*, so an absolute relocation into a data section is
# never applied. gcc emits exactly that for things like
#
#     static const char *msg   = "hello";
#     static const u8   *tab[] = { a, b, c };
#
# where the stored value would be a link-time offset rather than a real
# runtime address. On console that reads back as a wild pointer, and the only
# symptom is a hang -- so it is caught here instead.
#
# Fix: declare the pointer or table uninitialised and populate it at runtime.

READELF=${READELF:-readelf}
bad=0

for o in "$@"; do
    [ -f "$o" ] || continue
    out=$("$READELF" -r "$o" 2>/dev/null | awk '
        /^Relocation section/ { sec = $3; gsub(/'"'"'/, "", sec) }
        /R_X86_64_64/ {
            if (sec ~ /^\.rela\.(data|rodata)/) print "    " sec "  ->  " $5
        }' | sort -u)

    if [ -n "$out" ]; then
        echo "RELOC HAZARD in $o -- initialised pointer data:"
        echo "$out"
        bad=1
    fi
done

if [ "$bad" -ne 0 ]; then
    echo ""
    echo "These pointers would read as link-time offsets at runtime."
    echo "Declare them uninitialised and assign at runtime instead."
    exit 1
fi

echo "check: no relocation hazards"
