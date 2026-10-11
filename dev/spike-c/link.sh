#!/bin/bash
# Stream C spike: archive the replayed objects per library and link Mesa's
# standalone glsl_compiler for m68k-amigaos (GCC 16, libnix).
# Output: ~/spike-c/glsl_compiler, link.log, glsl_compiler.map, undefined.txt
S=${S:-$HOME/spike-c162}
P=${M68K_TC:-$HOME/amiga-gcc162-install}/bin/m68k-amigaos-
O=$S/obj
A=$S/lib
mkdir -p $A
rm -f $A/*.a

mk() {  # mk <name> <object dir>
    local name=$1 dir=$2
    find "$dir" -name '*.o' | sort > $A/$name.list
    ${P}ar rcs $A/lib$name.a $(cat $A/$name.list)
    echo "$name: $(wc -l < $A/$name.list) objects"
}
mk glsl_standalone $O/src/compiler/glsl/libglsl_standalone.a.p
mk glsl_util       $O/src/compiler/glsl/libglsl_util.a.p
mk glsl            $O/src/compiler/glsl/libglsl.a.p
mk glcpp           $O/src/compiler/glsl/glcpp/libglcpp.a.p
mk nir             $O/src/compiler/nir/libnir.a.p
mk compiler        $O/src/compiler/libcompiler.a.p
mk mesa_util       $O/src/util/libmesa_util.a.p
mk c11             $O/src/c11/impl/libmesa_util_c11.a.p
mk blake3          $O/src/util/blake3/libblake3.a.p
mk compat          $O/compat

# --allow-multiple-definition: bebbo GCC 16 links its own new_op.o, which defines
# __cxa_pure_virtual/__cxa_deleted_virtual like libstdc++'s pure.o does.
MAIN=$(find $O/src/compiler/glsl/glsl_compiler.p -name '*.o')
${P}g++ -mcpu=68020 -msoft-float -mcrt=nix20 -fno-exceptions -fno-rtti -o $S/glsl_compiler $MAIN \
    -L$A -Wl,--start-group -lglsl_standalone -lglsl_util -lglsl -lglcpp -lnir -lcompiler \
    -lmesa_util -lc11 -lblake3 -lcompat -Wl,--end-group \
    -lstdc++ -lpthread -latomic -lm -Wl,--allow-multiple-definition -Wl,-Map,$S/glsl_compiler.map > $S/link.log 2>&1
echo "link rc=$?"
grep -oE "undefined reference to \`[^']+'" $S/link.log | sed "s/undefined reference to \`//; s/'$//" \
    | sort | uniq -c | sort -rn > $S/undefined.txt
echo "distinct undefined symbols: $(wc -l < $S/undefined.txt)"
head -60 $S/undefined.txt
grep -vE "undefined reference|^\s*$|more undefined" $S/link.log | grep -iE "error|warning" | sort | uniq -c | head -10
ls -l $S/glsl_compiler 2>/dev/null
