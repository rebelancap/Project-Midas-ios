cmake_minimum_required(VERSION 3.16)

# GE_IOS: the Apple-mobile cross build (round 9a, docs/port-ios/16; adapted from
# GoldenEye-Decomp-ios engine/CMakeLists.txt). CMAKE_SYSTEM_NAME comes from the
# command line (scripts/build-ios.sh) before project(), so the decision is
# usable everywhere below. In a cross build NOTHING is searched for on the host
# (Homebrew SDL2 / libz are macOS dylibs): every dependency arrives as an
# explicit path. The C side gates on PLATFORM_IOS (port/include/platform.h,
# off TARGET_OS_IPHONE), not on this variable.
if(CMAKE_SYSTEM_NAME STREQUAL "iOS" OR CMAKE_SYSTEM_NAME STREQUAL "visionOS")
    set(GE_IOS TRUE)
else()
    set(GE_IOS FALSE)
endif()

project(ge007 C)

# GoldenEye PC port build (docs/port/PORT-PLAN.md). Built 32-bit first, which
# defuses the codebase's pointer<->s32 casts and keeps Gfx 8 bytes.
# The N64 ROM build stays on the classic Makefile; this file is PC-only.

set(CMAKE_C_STANDARD 11)

# Apple (macOS arm64 oracle, later iOS): native 64-bit AppleClang build. The
# x86 path below is untouched; every Apple difference is an additive
# if(APPLE) block (docs/port-ios/03-appleclang-spike.md).
if(APPLE)
    set(GE_HOST32_DEFAULT OFF)
else()
    set(GE_HOST32_DEFAULT ON)
endif()
option(GE_HOST32 "32-bit x86 host build (-m32 -msse2 -mfpmath=sse)" ${GE_HOST32_DEFAULT})
if(GE_HOST32)
    set(GE_ARCH_FLAGS -m32 -msse2 -mfpmath=sse)
    set(GE_WARN_FLAGS -w)
else()
    set(GE_ARCH_FLAGS)
    # AppleClang parity flags (GoldenEye-Decomp-ios docs/spike-clang.md):
    # no FMA contraction (x86 SSE never fuses), and keep sinf/cosf calls real.
    list(APPEND GE_ARCH_FLAGS -ffp-contract=off -fno-builtin-sinf -fno-builtin-cosf)
    # GE_WARN_CENSUS: drop -w and print the 64-bit pointer census instead.
    option(GE_WARN_CENSUS "Print the 64-bit pointer-width warning census" OFF)
    if(GE_WARN_CENSUS)
        set(GE_WARN_FLAGS
            $<$<COMPILE_LANGUAGE:C>:-Werror=int-conversion>
            -Wshorten-64-to-32 -Wpointer-to-int-cast
            -Wint-to-pointer-cast -Wvoid-pointer-to-int-cast
            -Wno-error=incompatible-function-pointer-types
            -Wno-error=incompatible-pointer-types
            $<$<COMPILE_LANGUAGE:C>:-Werror=implicit-function-declaration>
            -Wno-error=implicit-int
            $<$<COMPILE_LANGUAGE:CXX>:-Wno-error=c++11-narrowing>)
    else()
        # Phase 0: compile, do not fix. The pointer-truncation sites are the
        # Phase 1 work; these clang default-errors are silenced until then.
        set(GE_WARN_FLAGS -w
            # Phase 1 round 5 gate (docs/port-ios/08-round5-census.md): the
            # census is 0 for these two; any regression is a hard error.
            $<$<COMPILE_LANGUAGE:C>:-Werror=int-conversion>
            $<$<COMPILE_LANGUAGE:C>:-Wno-incompatible-function-pointer-types>
            $<$<COMPILE_LANGUAGE:C>:-Wno-incompatible-pointer-types>
            $<$<COMPILE_LANGUAGE:C>:-Werror=implicit-function-declaration>
            $<$<COMPILE_LANGUAGE:C>:-Wno-implicit-int>
            # fast3d: Gfx words are uintptr_t on LP64 (64-bit Gfx); narrowing
            # them into the u32 debug ring is the same Phase 1 class
            $<$<COMPILE_LANGUAGE:CXX>:-Wno-c++11-narrowing>)
    endif()
    # 64-bit host: the donor port's guards (GoldenEye-Decomp-ios, D-M02/D-M03
    # in docs/port-ios/00-PLAN.md). Midas itself has no PORT arms, so these
    # only enable imported donor code; the x86 build never sees them.
    add_compile_definitions(PORT=1 PLATFORM_64BIT=1)
    if(GE_IOS)
        # Round 9a: the Apple-mobile arms (docs/port-ios/16 R9A-1). Midas's own
        # iOS arms test GE_IOS, NOT PLATFORM_IOS: every Apple SDK's
        # <mach-o/loader.h> defines PLATFORM_IOS (=2, the LC_BUILD_VERSION
        # platform enum), so `#ifdef PLATFORM_IOS` is true on macOS in any TU
        # that includes a mach-o header (main.c does). Round 9c: PLATFORM_IOS
        # is no longer passed here (it collided with loader.h's 2, a
        # redefinition warning); the one dormant donor arm (src/game/lv.c) now
        # tests GE_IOS, and port/include/platform.h still derives PLATFORM_IOS
        # from TargetConditionals for the donor code that includes it.
        add_compile_definitions(GE_IOS=1)
    endif()
    # Donor shadow headers needed on 64-bit (PR/gbi.h's LE Gfx_le command
    # views), searched before include/. The rest of the donor's port/shim is
    # redundant with Midas's own headers (docs/port-ios/04-round1-prereqs.md).
    include_directories(BEFORE ${CMAKE_SOURCE_DIR}/port/apple/shim)
    # GE has its own sprintf (src/sprintf.c); the SDK's fortify macros in
    # <secure/_stdio.h> rewrite it into __builtin___sprintf_chk.
    add_compile_definitions(_FORTIFY_SOURCE=0)
endif()

add_compile_options(
    ${GE_ARCH_FLAGS}
    # Use SSE scalar floats so every operation rounds to single precision like
    # the N64 FPU; x87's 80-bit excess precision flips borderline comparisons
    # (it broke the stan.c tile-walk edge test and room visibility in Egypt).
    -fno-strict-aliasing
    -fwrapv
    -funsigned-char
    # crash-handler EBP-chain walk (port/src/main.c) needs real frame pointers
    -fno-omit-frame-pointer
    # DWARF in every build (incl. Windows PE): zero runtime cost, and it
    # upgrades crash symbolization from symbol+offset to file:line
    # (addr2line/gdb work on the exe directly)
    -g
    # -O2, keeping the flags that defuse decompiled-code UB (-fno-strict-aliasing
    # -fwrapv -funsigned-char -msse2 -mfpmath=sse); verified pixel-identical to
    # the old -Og build (see docs/port/optimisation-inventory.md).
    -O2
    # (Audit recipe: swap the -w below for -Wreturn-type and rebuild, since
    # falling off a non-void function is live UB at -O2. Keep the tree clean.)
    # GE_WARN_FLAGS is -w on the x86 build.
    ${GE_WARN_FLAGS}
    # C-only: game code is self-contained against repo-local headers;
    # fast3d C++ needs hosted stdlib and no ms-extensions quirks.
    $<$<COMPILE_LANGUAGE:C>:-ffreestanding>
    $<$<COMPILE_LANGUAGE:C>:-fms-extensions>
    $<$<COMPILE_LANGUAGE:C>:-fcommon>
)
# -no-pie: the compiled-in asset objects (below) are built -fno-pic so their
# pointer-laden .data needs no PIC indirection; a PIE final link would reject
# their R_386_32 relocations. MinGW PE has no PIE concept and its gcc driver
# rejects the flag, so it is ELF-only.
if(WIN32)
    # Statically link libstdc++/libgcc so the exe needs no extra DLLs, set the
    # image base above the N64 segment range so gfx_pc.cpp seg_addr can tell
    # addresses from segment refs, and use a 16MB stack (recursive room/portal
    # walks overflow 1MB).
    add_link_options(-m32 -static-libgcc -static-libstdc++
                     -Wl,--image-base=0x10000000 -Wl,--disable-dynamicbase
                     -Wl,--stack,16777216)
    link_libraries(dbghelp)
elseif(APPLE)
    # Mach-O arm64 is always PIE; no -m32/-no-pie.
else()
    add_link_options(-m32 -no-pie)
endif()

add_compile_definitions(
    VERSION_US LANG_US REFRESH_NTSC
    LEFTOVERDEBUG LEFTOVERSPECTRUM BUGFIX_R0
    NONMATCHING AVOID_UB _LANGUAGE_C
)
if(WIN32)
    # MinGW's ANSI-stdio layer defines static inline sprintf/printf wrappers
    # that collide with GE's own sprintf (src/sprintf.c, bespoke semantics).
    add_compile_definitions(__USE_MINGW_ANSI_STDIO=0)
endif()

# port/include: shim<->scheduler interface (port_libultra.h). src/libultra:
# lets vitbl.c resolve <io/viint.h>. Both appended LAST, so no include that
# resolved before can be shadowed.
include_directories(. include include/ultra64 include/PR src src/game src/inflate
                    port/include src/libultra)

# Game logic: everything in src/game.
file(GLOB GAME_SRC CONFIGURE_DEPENDS src/game/*.c)

# Engine layer: the src/*.c files the N64 build compiles, minus N64-only
# machinery the port deletes (boot chain, TLB pager, rmon, usb, crash handler).
file(GLOB ENGINE_SRC_ALL CONFIGURE_DEPENDS src/*.c)
set(ENGINE_EXCLUDE boot tlb_manage tlb_hardwire rmon usb debug crash motor)
# motor.c is GE's own osMotor* implementation over raw PIF-ram DMA - not
# backable by the shim; port/src/motor_stub.c provides the API.
set(ENGINE_SRC)
foreach(f ${ENGINE_SRC_ALL})
    get_filename_component(base ${f} NAME_WE)
    if(NOT base IN_LIST ENGINE_EXCLUDE)
        list(APPEND ENGINE_SRC ${f})
    endif()
endforeach()

# gu math: explicit list — only the files resolving actually-undefined gu/sins/
# coss symbols. Do NOT glob the dir: sqrtf.s/libm_vals.s are asm-backed, and
# sinf.c/cosf.c (fsin/fcos) are superseded by libm sinf/cosf (link -lm below).
set(GU_SRC
    src/libultra/gu/align.c        # guAlignF
    src/libultra/gu/coss.c         # coss (s16 sine-table cosine)
    src/libultra/gu/lookat.c       # guLookAt
    src/libultra/gu/lookatref.c    # guLookAtReflect
    src/libultra/gu/mtxutil.c      # guMtxF2L (+ guMtxIdentF used by ortho/perspective)
    src/libultra/gu/normalize.c    # guNormalize
    src/libultra/gu/ortho.c        # guOrtho
    src/libultra/gu/perspective.c  # guPerspective/guPerspectiveF
    src/libultra/gu/rotate.c       # guRotate/guRotateF
    src/libultra/gu/scale.c        # guScale/guScaleF
    src/libultra/gu/sins.c         # sins (s16 sine-table)
    src/libultra/gu/translate.c    # guTranslate
    # ---- AUDIO: the real AL synthesizer, which only builds Acmd lists that
    # port/src/audio_pc.c executes. bnkf.c is deliberately absent because
    # audio_stubs.c has the big-endian-aware alBnkfNew.
    src/libultra/audio/sl.c
    src/libultra/audio/synthesizer.c
    src/libultra/audio/event.c
    src/libultra/audio/heapinit.c
    src/libultra/audio/heapalloc.c
    src/libultra/audio/cents2ratio.c
    src/libultra/audio/copy.c
    src/libultra/audio/synaddplayer.c
    src/libultra/audio/synallocfx.c
    src/libultra/audio/synallocvoice.c
    src/libultra/audio/syndelete.c
    src/libultra/audio/synfreevoice.c
    src/libultra/audio/synsetfxmix.c
    src/libultra/audio/synsetpan.c
    src/libultra/audio/synsetpitch.c
    src/libultra/audio/synsetpriority.c
    src/libultra/audio/synsetvol.c
    src/libultra/audio/synstartvoice.c
    src/libultra/audio/synstartvoiceparam.c
    src/libultra/audio/synstopvoice.c
    src/libultra/audio/load.c
    src/libultra/audio/save.c
    src/libultra/audio/resample.c
    src/libultra/audio/filter.c
    src/libultra/audio/mainbus.c
    src/libultra/audio/auxbus.c
    src/libultrare/audio/drvrNew.c
    src/libultrare/audio/reverb.c
    src/libultrare/audio/env.c
    # MUSIC: the compressed-MIDI sequencer
    src/libultra/audio/cseq.c
    src/libultra/audio/csplayer.c
    src/libultra/audio/cspgetstate.c
    src/libultra/audio/cspplay.c
    src/libultra/audio/cspsetseq.c
    src/libultra/audio/cspsetvol.c
    src/libultra/audio/cspstop.c
    src/libultra/audio/seqpsetbank.c
    src/libultra/audio/seqplayer.c   # __lookupVoice/__vsVol/__initFromBank... shared voice machinery
    src/libultra/audio/seq.c
)

# ---------------------------------------------------------------------------
# Compiled-in assets.
#
# The N64 build compiles each asset .c into its own elf, so every Model.c reuses
# the same internal symbol names (proptextures, ModelNode_0x018, ...) and
# stan/setup/bg files reuse tile_0/padlist/header. Globbing them into one
# executable would cause thousands of duplicate definitions, so each asset TU is
# compiled separately and objcopy'd so only its file symbol (CarmourguardZ,
# Ump_setupameZ, ...) stays global:
#
#   DIRECT assets define their file symbol in the C; objcopy -G keeps just it.
#   SEG    assets (models, bg) don't define it anywhere (on N64 it's the
#          address of the linked blob). The TU is compiled with
#          -fno-toplevel-reorder -malign-data=abi -fno-common
#          -fno-zero-initialized-in-bss, which reproduces the N64 file layout
#          in .data byte-for-byte, and objcopy --add-symbol plants the file
#          symbol at .data offset 0. The layout must match because
#          load_object_fill_header finds Textures/RootNode by offset arithmetic.
#
# The per-file list is generated (port/generated/asset_manifest.cmake) by
# scripts/port/gen_asset_aliases.py. Rerun it if assets or the link surface
# change; it is not a configure-time glob.
#
# Known caveats: bg .c interior point tables, ANIM_DATA_* streams and
# oddtextures texel data are big-endian blobs.
set(ASSET_C_FLAGS
    -std=gnu11
    -m32 -malign-data=abi
    -fms-extensions -ffreestanding -fno-strict-aliasing -fwrapv -funsigned-char
    -fno-common -fno-zero-initialized-in-bss -fno-toplevel-reorder -fno-pic
    -Og -w
    -DVERSION_US -DLANG_US -DREFRESH_NTSC
    -DLEFTOVERDEBUG -DLEFTOVERSPECTRUM -DBUGFIX_R0
    -DNONMATCHING -DAVOID_UB -D_LANGUAGE_C
    -I${CMAKE_SOURCE_DIR}
    -I${CMAKE_SOURCE_DIR}/include
    -I${CMAKE_SOURCE_DIR}/include/ultra64
    -I${CMAKE_SOURCE_DIR}/include/PR
    -I${CMAKE_SOURCE_DIR}/src
    -I${CMAKE_SOURCE_DIR}/src/game
    -I${CMAKE_SOURCE_DIR}/src/inflate
    -I${CMAKE_SOURCE_DIR}/port/include
    -I${CMAKE_SOURCE_DIR}/src/libultra)

if(NOT CMAKE_OBJCOPY)
    set(CMAKE_OBJCOPY objcopy)
endif()
set(ASSET_OBJ_DIR ${CMAKE_BINARY_DIR}/asset_obj)
file(MAKE_DIRECTORY ${ASSET_OBJ_DIR})

# i386 PE/COFF prefixes C symbols with `_` (ELF doesn't), so add it here to the
# manifest's C-level names before objcopy uses them (like
# port/include/asmsym.h).
if(WIN32)
    set(ASSET_SYM_PREFIX "_")
else()
    set(ASSET_SYM_PREFIX "")
endif()

# NOTE: setup ai-list bytes are built from aicommands2.h's macros, so it must
# stay in these DEPENDS; if an AI macro edit has no effect, check the asset
# object is stale.
# Asset TU whose C defines the file symbol itself (stan/setup/brief/text):
# localize everything else.
# Every asset comes from the user's ROM except three inline-texture menu models
# (PORT_ASSET_OBJS_KEEP); with GE_ROMDATA_FALLBACK=OFF the rest are dropped and
# their weak file symbols resolve to NULL (ob_end.seg always lives in ob_seg.s).
set(PORT_ASSET_KEEP_SYMS PlegalpageZ PnintendologoZ PgoldeneyelogoZ)
function(port_asset_record obj sym)
    if(${sym} IN_LIST PORT_ASSET_KEEP_SYMS)
        set_property(GLOBAL APPEND PROPERTY PORT_ASSET_OBJS_KEEP ${obj})
    else()
        set_property(GLOBAL APPEND PROPERTY PORT_ASSET_OBJS_SERVED ${obj})
    endif()
endfunction()

function(port_add_asset_direct src sym)
    set(obj ${ASSET_OBJ_DIR}/${sym}.o)
    add_custom_command(OUTPUT ${obj}
        COMMAND ${CMAKE_C_COMPILER} ${ASSET_C_FLAGS} -c ${CMAKE_SOURCE_DIR}/${src} -o ${obj}.raw
        COMMAND ${CMAKE_OBJCOPY} -G ${ASSET_SYM_PREFIX}${sym} ${obj}.raw ${obj}
        DEPENDS ${CMAKE_SOURCE_DIR}/${src}
                ${CMAKE_SOURCE_DIR}/src/bondtypes.h
                ${CMAKE_SOURCE_DIR}/src/bondconstants.h
                ${CMAKE_SOURCE_DIR}/src/bondaicommands.h
                ${CMAKE_SOURCE_DIR}/src/aicommands2.h
        COMMENT "asset ${sym}" VERBATIM)
    port_asset_record(${obj} ${sym})
endfunction()

# Asset TU with no file symbol of its own (Model.c, bg .c): localize all
# internals and plant the file symbol at .data offset 0 (the file start).
function(port_add_asset_seg src sym)
    set(obj ${ASSET_OBJ_DIR}/${sym}.o)
    add_custom_command(OUTPUT ${obj}
        COMMAND ${CMAKE_C_COMPILER} ${ASSET_C_FLAGS} -c ${CMAKE_SOURCE_DIR}/${src} -o ${obj}.raw
        COMMAND ${CMAKE_OBJCOPY} -G ${ASSET_SYM_PREFIX}${sym} --add-symbol ${ASSET_SYM_PREFIX}${sym}=.data:0,global,object ${obj}.raw ${obj}
        DEPENDS ${CMAKE_SOURCE_DIR}/${src}
                ${CMAKE_SOURCE_DIR}/src/bondtypes.h
                ${CMAKE_SOURCE_DIR}/src/bondconstants.h
                ${CMAKE_SOURCE_DIR}/src/bondaicommands.h
                ${CMAKE_SOURCE_DIR}/src/aicommands2.h
        COMMENT "asset ${sym}" VERBATIM)
    port_asset_record(${obj} ${sym})
endfunction()

# GE_COMPILED_ASSETS OFF (default on Apple): skip the objcopy'd asset objects
# entirely - their -m32 -malign-data=abi layout tricks are GCC/ELF/PE-only.
if(APPLE)
    set(GE_COMPILED_ASSETS_DEFAULT OFF)
else()
    set(GE_COMPILED_ASSETS_DEFAULT ON)
endif()
option(GE_COMPILED_ASSETS "Build the objcopy'd compiled-in asset objects" ${GE_COMPILED_ASSETS_DEFAULT})
if(GE_COMPILED_ASSETS)
include(${CMAKE_SOURCE_DIR}/port/generated/asset_manifest.cmake)
endif()
if(NOT GE_COMPILED_ASSETS AND NOT GE_HOST32)
    # No asset objects: define obseg.h's weak asset file symbols as NULL in a
    # generated TU (ld64 will not NULL a weak undefined reference itself).
    set(GE_WEAK_ASSET_NULL_C ${CMAKE_BINARY_DIR}/weak_asset_null.c)
    add_custom_command(OUTPUT ${GE_WEAK_ASSET_NULL_C}
        COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${GE_WEAK_ASSET_NULL_C}
                -P ${CMAKE_SOURCE_DIR}/scripts/port/gen_weak_asset_null.cmake
        DEPENDS ${CMAKE_SOURCE_DIR}/assets/obseg/obseg.h
                ${CMAKE_SOURCE_DIR}/scripts/port/gen_weak_asset_null.cmake
                ${CMAKE_SOURCE_DIR}/port/src/ucode_dl_stubs.c
        COMMENT "weak asset NULL TU" VERBATIM)
endif()
get_property(ASSET_OBJS_KEEP GLOBAL PROPERTY PORT_ASSET_OBJS_KEEP)
get_property(ASSET_OBJS_SERVED GLOBAL PROPERTY PORT_ASSET_OBJS_SERVED)
# GE_ROMDATA_FALLBACK OFF: drop the served asset objects (models/bg/setups/
# stans/text/briefs) - the exe then carries none of Rare's data and the ROM
# is mandatory. ON keeps them for the GE_ROMDATA=0 / missing-ROM fallback.
if(GE_ROMDATA_FALLBACK)
    set(ASSET_OBJS ${ASSET_OBJS_KEEP} ${ASSET_OBJS_SERVED})
else()
    set(ASSET_OBJS ${ASSET_OBJS_KEEP})
    message(STATUS "GE_ROMDATA_FALLBACK=OFF: dropping the served asset objects from the exe (ROM mandatory)")
endif()
set_source_files_properties(${ASSET_OBJS} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)

# Loose single-TU asset data (unique symbol names; compiled like game code):
set(ASSET_LOOSE_SRC
    assets/oddtextures.c         # s_*image + globalDL_* (texel data is BE: renders wrong until swapped; resolves image_bank externs)
    assets/rarewarelogo.c        # logo data; only reachable once _rarewarelogoSegment* aliases land (romdata wave)
    assets/font_dl.c             # dlBasicGeometry / dlZBufferGeometry / dlFastPipelineSetup: front.c/lv.c/title.c
                                 # gSPDisplayList(&dl...) them BY SYMBOL, so they stay in every build (the fontdl
                                 # SEGMENT copy comes off the ROM); 184 bytes
)
# ROM loading (docs/port/romdata.md): animation + fonts come off the
# user's ROM; these compiled copies stay only for GE_ROMDATA_FALLBACK (below).
set(ASSET_LOOSE_FALLBACK_SRC
    assets/animationtable_data.c    # animation_data segment blob (see animdata.c)
    assets/animationtable_entries.c # animation frame streams (see animdata.c)
    assets/font/fontBankGothic.c # TEXT WAVE: kerning + chartable + glyph bytes (composed by portFontBuildSegments)
    assets/font/fontZurichBold.c # TEXT WAVE: chartable + glyph bytes (kerning not extracted; zeros)
)
# animdata.c rebuilds the N64 animation segment images from these blobs by
# offset arithmetic - they must tile contiguously in declaration order.
# Kept at -Og with the tree at -O2: data-only TUs, layout is the contract.
set_source_files_properties(assets/animationtable_data.c assets/animationtable_entries.c
    PROPERTIES COMPILE_OPTIONS "-fno-toplevel-reorder;-malign-data=abi;-Og")
# ---------------------------------------------------------------------------

# Port layer (grows as PD-derived pieces land; see docs/port/).
file(GLOB PORT_SRC CONFIGURE_DEPENDS port/src/*.c)
if(NOT GE_HOST32)
    # 64-bit host layer (docs/port-ios/04-round1-prereqs.md): guest window,
    # donor hook stubs. Never part of the 32-bit x86 build.
    file(GLOB PORT_APPLE_SRC CONFIGURE_DEPENDS port/apple/*.c)
    list(APPEND PORT_SRC ${PORT_APPLE_SRC})
    # Round 10 (docs/port-ios/19-round10-xbla-import.md, XBLA step 1): the
    # donor's XBLA importer (port/apple/{xblaimport,x360,beantex,dxt,pngread,
    # pngwrite}.c, picked up by the glob above) reads .7z releases through
    # Igor Pavlov's LZMA SDK, vendored as-is under port/apple/external/lzma.
    file(GLOB PORT_APPLE_LZMA_SRC CONFIGURE_DEPENDS port/apple/external/lzma/*.c)
    list(APPEND PORT_SRC ${PORT_APPLE_LZMA_SRC})
    # The importer and its config shim are host malloc only and never see the
    # sim's allocators (donor D-110 discipline, its CMakeLists Tier C check):
    # an include of mema.h / memp.h in any of them fails the configure.
    # Plain host C with no game types, but the decomp's include/ is on the -I
    # path and its C-side string.h stub lacks strrchr & co. As the donor does
    # (its CMakeLists "SRC_PORT_HOSTLIBC"), force the HOST <string.h> in first
    # by absolute path: it defines the same _STRING_H_ guard, so the stub then
    # contributes nothing. Only these TUs; the game's are untouched.
    if(APPLE)
        if(CMAKE_OSX_SYSROOT AND IS_DIRECTORY "${CMAKE_OSX_SYSROOT}")
            set(GE_XBLA_SDK "${CMAKE_OSX_SYSROOT}")
        elseif(GE_IOS)
            execute_process(COMMAND xcrun --sdk ${CMAKE_OSX_SYSROOT} --show-sdk-path
                OUTPUT_VARIABLE GE_XBLA_SDK OUTPUT_STRIP_TRAILING_WHITESPACE)
        else()
            execute_process(COMMAND xcrun --show-sdk-path
                OUTPUT_VARIABLE GE_XBLA_SDK OUTPUT_STRIP_TRAILING_WHITESPACE)
        endif()
        set(GE_XBLA_HOST_STRING_H "${GE_XBLA_SDK}/usr/include/string.h")
    else()
        set(GE_XBLA_HOST_STRING_H "/usr/include/string.h")
    endif()
    set(GE_XBLA_HOSTLIBC_SRC ${PORT_APPLE_LZMA_SRC})
    # Round 13: beanmesh.c is plain host C like beantex.c (the donor's
    # SRC_PORT_HOSTLIBC lists both); beanstage.c includes the game headers and
    # is compiled as the donor does, without the forced host string.h.
    foreach(_xbla xblaimport.c x360.c beantex.c dxt.c pngread.c pngwrite.c
                  xblaconfig.c xbla_keys.c xblacli.c beanmesh.c xblafont.c)
        list(APPEND GE_XBLA_HOSTLIBC_SRC "${CMAKE_SOURCE_DIR}/port/apple/${_xbla}")
    endforeach()
    set_source_files_properties(${GE_XBLA_HOSTLIBC_SRC} PROPERTIES COMPILE_OPTIONS
        "-include;${GE_XBLA_HOST_STRING_H}")
    # Round 11: texpack.c (XBLA step 2) is held to the same rule: host
    # malloc plus one piece of the dedicated guest bump, never mema/memp.
    # Round 13: beanmesh.c and beanstage.c (XBLA step 4.1) are host malloc
    # only (beanstage.c also #errors if either header arrives transitively).
    # Round 14: beanmodel.c and beangun.c (XBLA steps 4.2/4.3) likewise (both
    # #error on a transitive mema.h/memp.h too).
    # Round 15: beanchr.c and beanprefetch.c (XBLA step 4.4) likewise.
    foreach(_xbla xblaimport.c x360.c beantex.c dxt.c pngread.c pngwrite.c
                  xblaconfig.c xbla_keys.c xblacli.c texpack.c beanmesh.c beanstage.c
                  beanmodel.c beangun.c beanchr.c beanprefetch.c xblafont.c)
        set(_xbla_path "${CMAKE_SOURCE_DIR}/port/apple/${_xbla}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_xbla_path}")
        file(STRINGS "${_xbla_path}" _xbla_hits REGEX "^[ \t]*#[ \t]*include[ \t]*[<\"]([^>\"]*/)?mem[ap]\\.h[>\"]")
        if(_xbla_hits)
            message(FATAL_ERROR "XBLA file ${_xbla} includes mema.h/memp.h: ${_xbla_hits} (host malloc only)")
        endif()
    endforeach()
    # Round 7b (docs/port-ios/12-round7b-boot.md): models, bgs, stans and
    # setups come from the pcconvert sidecars in the 64-bit host layout, so
    # Midas's in-place 32-bit converters are x86-only (the host test
    # pcconvert_test still compiles three of them as its reference).
    list(REMOVE_ITEM PORT_SRC
        ${CMAKE_SOURCE_DIR}/port/src/romfile_model.c
        ${CMAKE_SOURCE_DIR}/port/src/romfile_setup.c
        ${CMAKE_SOURCE_DIR}/port/src/romfile_stan.c
        ${CMAKE_SOURCE_DIR}/port/src/romfile_bg.c)
    # Round 6 (R1, donor D32; docs/port-ios/09-round6-shim.md): GE's
    # gzip-1.2.4 inflate (decompress.c + zlib.c) is replaced on 64-bit hosts by
    # port/apple/rzdecomp.c over the system zlib. `struct huft` doubles to 16 B
    # on LP64 and the hand-rolled inflate's globals/tables are 32-bit-shaped;
    # real zlib owns its own table memory. The x86 build keeps Midas's files.
    list(REMOVE_ITEM GAME_SRC
        ${CMAKE_SOURCE_DIR}/src/game/decompress.c
        ${CMAKE_SOURCE_DIR}/src/game/zlib.c)
    find_package(ZLIB REQUIRED)
    # src/game/zlib.h sits on the -I path and shadows <zlib.h>, so rzdecomp.c
    # includes the host header by absolute path through a generated wrapper.
    list(GET ZLIB_INCLUDE_DIRS 0 GE_REAL_ZLIB_DIR)
    set(GE_REAL_ZLIB_H "${GE_REAL_ZLIB_DIR}/zlib.h")
    configure_file(${CMAKE_SOURCE_DIR}/port/apple/realzlib.h.in
        ${CMAKE_BINARY_DIR}/generated/realzlib.h @ONLY)
endif()

# GE_HEADLESS: no SDL at all (null renderer, silent audio), for the Linux -m32
# ASan build where no 32-bit SDL exists. GE_SANITIZE adds ASan+UBSan (Linux gcc
# only).
option(GE_HEADLESS "Build without SDL (null renderer, no audio)" OFF)
option(GE_SANITIZE "AddressSanitizer + UBSan build (implies headless)" OFF)
if(GE_SANITIZE)
    # Only default to headless when GE_VIDEO wasn't explicitly requested -
    # a GE_VIDEO=ON + GE_SANITIZE=ON build (real fast3d/SDL2 renderer,
    # e.g. via Docker's from-source 32-bit SDL2) must NOT also pull in
    # headless_stubs.c, or duplicate-definition link errors result
    # (ge_sky_push, gfx_texture_cache_* defined in both gfx_pc.cpp and
    # headless_stubs.c).
    if(NOT GE_VIDEO)
        set(GE_HEADLESS ON)
    endif()
    # -g3 gives file:line in reports and no-sanitize-recover=all makes every UBSan
    # finding fatal; if deliberate decomp UB floods it, exempt specific checks here.
    add_compile_options(-g3 -fsanitize=address -fsanitize=undefined
                        -fno-omit-frame-pointer -fno-sanitize-recover=all)
    add_link_options(-fsanitize=address -fsanitize=undefined)
    # ASan redzones put gaps between globals and break animdata's contiguous
    # tiling, so exempt the data-only TUs (manifest assets are already
    # uninstrumented).
    set_source_files_properties(assets/animationtable_data.c assets/animationtable_entries.c
        PROPERTIES COMPILE_OPTIONS "-fno-toplevel-reorder;-malign-data=abi;-Og;-fno-sanitize=all")
    set_source_files_properties(assets/oddtextures.c assets/rarewarelogo.c
        assets/font_dl.c assets/font/fontBankGothic.c assets/font/fontZurichBold.c
        PROPERTIES COMPILE_OPTIONS "-fno-sanitize=all")
endif()
if(GE_HEADLESS)
    list(REMOVE_ITEM PORT_SRC
        ${CMAKE_SOURCE_DIR}/port/src/audio_pc.c
        ${CMAKE_SOURCE_DIR}/port/src/video.c
        ${CMAKE_SOURCE_DIR}/port/src/input_pc.c
        ${CMAKE_SOURCE_DIR}/port/src/raphnet.c)
else()
    list(REMOVE_ITEM PORT_SRC ${CMAKE_SOURCE_DIR}/port/src/headless_stubs.c)
endif()
# segments.c .incbin's the raw image bank (texture wave); ramrom_data.c
# .incbin's the gunbarrel swirl RLE blob and the 14 attract-demo
# recordings. Give the assembler the include paths and re-assemble when
# the blobs change.
if(GE_COMPILED_ASSETS)
set_source_files_properties(port/src/segments.c PROPERTIES
    COMPILE_OPTIONS "-Wa,-I${CMAKE_SOURCE_DIR}/assets/images/combined;-Wa,-I${CMAKE_SOURCE_DIR}/assets/music"
    OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/assets/images/combined/combined.bin;${CMAKE_SOURCE_DIR}/assets/music/sfx.ctl;${CMAKE_SOURCE_DIR}/assets/music/sfx.tbl;${CMAKE_SOURCE_DIR}/assets/music/instruments.ctl;${CMAKE_SOURCE_DIR}/assets/music/instruments.tbl;${CMAKE_SOURCE_DIR}/assets/music/music.sbk")
set(PORT_RAMROM_DEMO_BINS
    ramrom_Dam_1 ramrom_Dam_2 ramrom_Facility_1 ramrom_Facility_2
    ramrom_Facility_3 ramrom_Runway_1 ramrom_Runway_2 ramrom_BunkerI_1
    ramrom_BunkerI_2 ramrom_Silo_1 ramrom_Silo_2 ramrom_Frigate_1
    ramrom_Frigate_2 ramrom_Train)
list(TRANSFORM PORT_RAMROM_DEMO_BINS PREPEND ${CMAKE_SOURCE_DIR}/assets/ramrom/)
list(TRANSFORM PORT_RAMROM_DEMO_BINS APPEND .bin)
set_source_files_properties(port/src/ramrom_data.c PROPERTIES
    COMPILE_OPTIONS "-Wa,-I${CMAKE_SOURCE_DIR}/assets"
    OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/assets/ge007.u.2A4D50.usedby7F008DE4.bin;${PORT_RAMROM_DEMO_BINS}")
endif() # GE_COMPILED_ASSETS (the .incbin blobs are ROM extracts)
# osViModeTable[42]: compile the repo's REAL table (fr.c/sched.c read
# comRegs.hStart/width/xScale + fldRegs[].vStart/yScale/origin from it)
# instead of transcribing data into the shim.
list(APPEND PORT_SRC src/libultrare/io/vitbl.c)

# ---------------------------------------------------------------------------
# Video: fast3d interpreter + SDL2/OpenGL. OFF by default until the
# GE microcode opcode work lands (docs/port/ge-microcode.md); the null
# renderer in pcsched.c stays the headless default.
if(APPLE)
    option(GE_VIDEO "Build the fast3d renderer (SDL2 + OpenGL)" ON)
    if(GE_IOS AND NOT GE_VIDEO)
        message(FATAL_ERROR "GE_IOS: the iOS engine is always the video build (GE_VIDEO=ON)")
    endif()
else()
    option(GE_VIDEO "Build the fast3d renderer (SDL2 + OpenGL)" OFF)
endif()

# Testing convenience: every solo mission, bonus stage and difficulty
# (incl. 007 mode) selectable regardless of save state. Progression logic
# untouched otherwise - saves still record normally.
option(GE_UNLOCK_ALL "Unlock all missions and difficulties (testing)" OFF)
if(GE_UNLOCK_ALL)
    add_compile_definitions(GE_UNLOCK_ALL=1)
endif()

# By default the exe carries none of Rare's data and exits without the user's ROM.
# -DGE_ROMDATA_FALLBACK=ON keeps compiled-in copies as a no-ROM fallback (see docs/port/romdata.md).
option(GE_ROMDATA_FALLBACK "Keep compiled-in copies of the ROM-served asset classes (GE_ROMDATA=0 / missing-ROM fallback)" OFF)
if(GE_ROMDATA_FALLBACK)
    set_source_files_properties(port/src/segments.c port/src/ramrom_data.c port/src/romdata.c port/src/animdata.c
        PROPERTIES COMPILE_DEFINITIONS GE_ROMDATA_FALLBACK=1)
    list(APPEND ASSET_LOOSE_SRC ${ASSET_LOOSE_FALLBACK_SRC})
endif()

if(GE_VIDEO)
    enable_language(CXX)
    set(CMAKE_CXX_STANDARD 20) # gfx_cc.h uses operator<=> (PD builds C++20)
    set(FAST3D_SRC
        port/fast3d/gfx_pc.cpp
        port/fast3d/gfx_cc.cpp
        port/fast3d/gfx_opengl.cpp
        port/fast3d/gfx_sdl2.cpp
        port/fast3d/glad/glad.c
    )
    add_compile_definitions(GE_VIDEO=1)
    # Round 13: gfx_pc.cpp owns the hdmesh entry points; the headless stand-ins
    # are for GE_VIDEO=OFF only.
    list(REMOVE_ITEM PORT_SRC ${CMAKE_SOURCE_DIR}/port/apple/hdmesh_headless.c)
    if(GE_IOS)
        # Round 12 (docs/port-ios/21): the GE_EXT_PROBE harness is a macOS
        # video build dev tool only, never in the iOS engine; gfx_pc.cpp's weak
        # geExtProbeList() returns NULL in its place.
        list(REMOVE_ITEM PORT_SRC ${CMAKE_SOURCE_DIR}/port/apple/extprobe.c)
    endif()
    if(APPLE)
        # libc++ hard-errors when the decomp's include/ (math.h, stdlib.h,
        # string.h, ... stubs) shadows the C headers its <cmath>/<cstdlib>
        # wrappers include_next into (GoldenEye-Decomp-ios spike-clang). For
        # C++ TUs only: put libc++ first, then a shim dir routing each shadowed
        # name to the host header by absolute path, then the normal includes.
        # C TUs are untouched.
        if(CMAKE_OSX_SYSROOT AND IS_DIRECTORY "${CMAKE_OSX_SYSROOT}")
            set(GE_APPLE_SDK "${CMAKE_OSX_SYSROOT}")
        elseif(GE_IOS)
            # CMAKE_OSX_SYSROOT is an SDK NAME here (iphonesimulator/iphoneos);
            # plain `xcrun --show-sdk-path` would answer with the macOS SDK.
            execute_process(COMMAND xcrun --sdk ${CMAKE_OSX_SYSROOT} --show-sdk-path
                OUTPUT_VARIABLE GE_APPLE_SDK OUTPUT_STRIP_TRAILING_WHITESPACE)
        else()
            execute_process(COMMAND xcrun --show-sdk-path
                OUTPUT_VARIABLE GE_APPLE_SDK OUTPUT_STRIP_TRAILING_WHITESPACE)
        endif()
        execute_process(COMMAND ${CMAKE_CXX_COMPILER} -print-resource-dir
            OUTPUT_VARIABLE GE_CLANG_RESOURCE_DIR OUTPUT_STRIP_TRAILING_WHITESPACE)
        set(GE_CXX_SHIM ${CMAKE_BINARY_DIR}/cxxshim)
        foreach(h math.h stdlib.h string.h assert.h limits.h)
            file(WRITE ${GE_CXX_SHIM}/${h} "#include \"${GE_APPLE_SDK}/usr/include/${h}\"\n")
        endforeach()
        foreach(h stddef.h stdarg.h)
            file(WRITE ${GE_CXX_SHIM}/${h} "#include \"${GE_CLANG_RESOURCE_DIR}/include/${h}\"\n")
        endforeach()
        set(CMAKE_CXX_COMPILE_OBJECT
            "<CMAKE_CXX_COMPILER> -nostdinc++ -I${GE_APPLE_SDK}/usr/include/c++/v1 -I${GE_CXX_SHIM} <DEFINES> <INCLUDES> <FLAGS> -o <OBJECT> -c <SOURCE>")
    endif()
else()
    set(FAST3D_SRC)
    # video.c/input_pc.c/raphnet.c are GE_VIDEO-only; exclude them from the port glob.
    # Round 12: so is the GE_EXT_PROBE harness (no renderer here).
    list(REMOVE_ITEM PORT_SRC ${CMAKE_SOURCE_DIR}/port/apple/extprobe.c)
    list(REMOVE_ITEM PORT_SRC
        ${CMAKE_SOURCE_DIR}/port/src/video.c
        ${CMAKE_SOURCE_DIR}/port/src/input_pc.c
        ${CMAKE_SOURCE_DIR}/port/src/raphnet.c)
endif()

# GE_GL_ANGLE (round 9a, from GoldenEye-Decomp-ios): the GL ES 3.0 context comes
# from ANGLE (EGL, Metal backend) instead of SDL - iOS has no desktop GL and
# EAGL is deprecated. The glue (app/gfx/gfx_angle_egl.mm) is ObjC++ outside the
# engine tree. With the option OFF nothing here exists and the macOS build keeps
# SDL's own GL path.
option(GE_GL_ANGLE "Create the GL ES context through ANGLE/EGL instead of SDL" OFF)
if(GE_GL_ANGLE)
    if(NOT GE_VIDEO)
        message(FATAL_ERROR "GE_GL_ANGLE needs GE_VIDEO=ON")
    endif()
    if(NOT GE_ANGLE_DIR OR NOT GE_ANGLE_INCLUDE_DIR OR NOT GE_ANGLE_GLUE)
        message(FATAL_ERROR "GE_GL_ANGLE needs GE_ANGLE_DIR, GE_ANGLE_INCLUDE_DIR and GE_ANGLE_GLUE")
    endif()
    if(NOT EXISTS "${GE_ANGLE_DIR}/libEGL.dylib" AND NOT EXISTS "${GE_ANGLE_DIR}/libEGL.framework")
        message(FATAL_ERROR "GE_GL_ANGLE: no libEGL.dylib and no libEGL.framework in ${GE_ANGLE_DIR}")
    endif()
    if(NOT EXISTS "${GE_ANGLE_GLUE}")
        message(FATAL_ERROR "GE_GL_ANGLE: no glue at ${GE_ANGLE_GLUE}")
    endif()
    add_compile_definitions(GE_GL_ANGLE=1)
endif()

# GE_VISION_3D (round 45, Phase 6 stereo 3D, docs/port-ios/64): the visionOS
# 3D mode's engine half. scripts/build-ios.sh passes ON for the xrsimulator and
# visionos slices only; the iOS and Mac builds never see it. M1 defines the
# macro and nothing reads it yet; M2/M3 guard the eye-target redirect and the
# per-eye projection fold with it.
option(GE_VISION_3D "visionOS stereo 3D engine seams (Phase 6)" OFF)
if(GE_VISION_3D)
    add_compile_definitions(GE_VISION_3D=1)
endif()

# GE_STATIC_LIB (round 9a): the whole engine as libmidas.a instead of an
# executable. CMake owns the engine cross-build, xcodegen/Xcode own the bundle
# (the donor's division of labour); main() moves to the shell and the engine's
# entry point is geEngineMain (port/src/main.c, PLATFORM_IOS).
option(GE_STATIC_LIB "Build the engine as a static library (libmidas.a)" OFF)
if(GE_STATIC_LIB)
    add_library(ge STATIC ${GAME_SRC} ${ENGINE_SRC} ${GU_SRC} ${PORT_SRC}
                ${ASSET_LOOSE_SRC} ${ASSET_OBJS} ${FAST3D_SRC}
                ${GE_WEAK_ASSET_NULL_C})
    set_target_properties(ge PROPERTIES OUTPUT_NAME midas)
else()
add_executable(ge ${GAME_SRC} ${ENGINE_SRC} ${GU_SRC} ${PORT_SRC}
               ${ASSET_LOOSE_SRC} ${ASSET_OBJS} ${FAST3D_SRC}
               ${GE_WEAK_ASSET_NULL_C})
# The executable is named ProjectMidas, but the CMake target stays `ge` so build
# scripts keep working, and ge_direct.log/Profiles keep their names.
set_target_properties(ge PROPERTIES OUTPUT_NAME ProjectMidas)
endif()
if(GE_GL_ANGLE)
    # Its own OBJECT target so it can drop the directory's include path: the
    # decomp's include/ shadows <math.h>/<stdlib.h>/<string.h>, which the glue
    # (host ObjC++ against SDL + EGL) must not see. Its objects go into `ge`.
    enable_language(OBJCXX)
    add_library(midas_angle_glue OBJECT ${GE_ANGLE_GLUE})
    set_property(TARGET midas_angle_glue PROPERTY INCLUDE_DIRECTORIES
        ${GE_ANGLE_INCLUDE_DIR} ${SDL2_INCLUDE_DIR})
    # -O2, no ARC: exactly how the donor's CMake builds the same file.
    set_property(TARGET midas_angle_glue PROPERTY COMPILE_OPTIONS -O2 -g)
    target_sources(ge PRIVATE $<TARGET_OBJECTS:midas_angle_glue>)
    if(EXISTS "${GE_ANGLE_DIR}/libEGL.dylib")
        target_link_libraries(ge "${GE_ANGLE_DIR}/libEGL.dylib" "${GE_ANGLE_DIR}/libGLESv2.dylib")
    else()
        # Framework slices: the app target embeds them; a static library only
        # records them as an interface dependency.
        target_link_libraries(ge "-F${GE_ANGLE_DIR}" "-framework libEGL" "-framework libGLESv2")
    endif()
    target_link_libraries(ge "-framework QuartzCore" "-framework Metal" "-framework Foundation")
endif()
# The exe's own icon (Explorer, a pinned taskbar entry): port/win/ProjectMidas.ico,
# generated with the window icon by scripts/port/make_icon.py.
if(WIN32)
    enable_language(RC)
    target_sources(ge PRIVATE ${CMAKE_SOURCE_DIR}/port/win/ProjectMidas.rc)
    # No console window: the GUI subsystem, keeping main() as the entry (unlike
    # -mwindows, which wants WinMain). The log goes to ge_direct.log instead
    # (main.c portDirectLaunchLog); redirected runs keep their output.
    target_link_options(ge PRIVATE -Wl,--subsystem,windows)
endif()

# Project Midas build stamp: <git commit count>-<YYYYMMDD>, regenerated on
# every build (scripts/port/buildinfo.cmake has the why). sysstubs.c owns the
# string; the legal screen and the `ge: sys: build` log line both read it.
add_custom_target(ge_buildinfo
    COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR}
            -DOUT=${CMAKE_BINARY_DIR}/generated/port_buildinfo.h
            -P ${CMAKE_SOURCE_DIR}/scripts/port/buildinfo.cmake
    BYPRODUCTS ${CMAKE_BINARY_DIR}/generated/port_buildinfo.h
    COMMENT "build stamp"
    VERBATIM)
add_dependencies(ge ge_buildinfo)
target_include_directories(ge PRIVATE ${CMAKE_BINARY_DIR}/generated)

# libm: sinf/cosf/sqrtf (math_sincos.s and gu asm on N64; libm on PC).
target_link_libraries(ge m)
if(NOT GE_HOST32)
    # port/apple/rzdecomp.c (round 6, R1)
    target_link_libraries(ge ZLIB::ZLIB)

    # Phase 1 round 7a (docs/port-ios/11-round7a-pcconvert.md): the donor's
    # pcconvert sidecar converters (d43 models, d69 bg/stan, d88 setups).
    # `pcconvert` is a host-libc library -- no game include path, so the
    # repo's include/ stubs never shadow <stdlib.h>/<string.h>/<stdio.h> --
    # over embedded build-time tables from the committed metadata.
    include(${CMAKE_SOURCE_DIR}/cmake/gen_pcconvert_tables.cmake)
    set(PCCONVERT_DIR ${CMAKE_SOURCE_DIR}/port/apple/pcconvert)
    # GE_STATIC_LIB: an OBJECT library, so its objects land inside libmidas.a
    # (a separate libpcconvert.a would be one more archive for the app to link).
    if(GE_STATIC_LIB)
        set(GE_PCCONVERT_KIND OBJECT)
    else()
        set(GE_PCCONVERT_KIND STATIC)
    endif()
    add_library(pcconvert ${GE_PCCONVERT_KIND}
        ${PCCONVERT_DIR}/pcconvert.c
        ${PCCONVERT_DIR}/pcconvert_util.c
        ${PCCONVERT_DIR}/d43.c
        ${PCCONVERT_DIR}/d69.c
        ${PCCONVERT_DIR}/d88.c
        ${PCCONVERT_DIR}/d88_propdefs.c
        ${SRC_PCCONVERT_TABLES})
    set_property(TARGET pcconvert PROPERTY INCLUDE_DIRECTORIES
        ${PCCONVERT_DIR} ${CMAKE_BINARY_DIR}/generated)
    target_compile_options(pcconvert PRIVATE -Wall -Werror)
    target_link_libraries(pcconvert PUBLIC ZLIB::ZLIB)
    # D-M05 pin: every size/offset the converters write, asserted against
    # Midas's headers inside the game build.
    target_sources(ge PRIVATE ${PCCONVERT_DIR}/pcconvert_layout.c)
    # The game-side server (pcmodels.h/pccg.h over the file table), linked
    # since round 7b (docs/port-ios/12-round7b-boot.md): romdata.c converts
    # at boot and loads both images; obInit patches the file table.
    target_sources(ge PRIVATE ${PCCONVERT_DIR}/pcsidecar.c)
    target_include_directories(ge PRIVATE ${PCCONVERT_DIR})
    target_link_libraries(ge pcconvert)
    # Host test: converts Austin's ROM in memory and validates the sidecars
    # (port/apple/tests/run_pcconvert_test.sh). Silent, no SDL. Host-only: not
    # part of a cross build.
    if(NOT GE_IOS)
    add_executable(pcconvert_test
        ${CMAKE_SOURCE_DIR}/port/apple/tests/pcconvert_test.c
        ${CMAKE_SOURCE_DIR}/port/src/romfile_model.c
        ${CMAKE_SOURCE_DIR}/port/src/romfile_stan.c
        ${CMAKE_SOURCE_DIR}/port/src/romfile_bg.c)
    target_include_directories(pcconvert_test PRIVATE ${PCCONVERT_DIR})
    target_link_libraries(pcconvert_test pcconvert)
    endif()
endif()

if(GE_VIDEO)
    if(WIN32)
        # the SDL2 MinGW development package: third_party/ by default, or
        # SDL2_MINGW_ROOT (the Docker image unpacks it in the home folder)
        set(SDL2_MINGW_ROOT "" CACHE PATH "SDL2 MinGW package's i686-w64-mingw32 folder (default: third_party/SDL2-2.30.11/i686-w64-mingw32)")
        if(SDL2_MINGW_ROOT)
            set(SDL2_ROOT ${SDL2_MINGW_ROOT})
        else()
            set(SDL2_ROOT ${CMAKE_SOURCE_DIR}/third_party/SDL2-2.30.11/i686-w64-mingw32)
        endif()
        target_include_directories(ge PRIVATE ${SDL2_ROOT}/include ${SDL2_ROOT}/include/SDL2)
        target_link_libraries(ge ${SDL2_ROOT}/lib/libSDL2.dll.a opengl32 gdi32 imm32 winmm setupapi hid version ole32 oleaut32)
        # the game needs SDL2.dll beside the exe
        add_custom_command(TARGET ge POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SDL2_ROOT}/bin/SDL2.dll $<TARGET_FILE_DIR:ge>)
    elseif(GE_IOS)
        # Static SDL2 by explicit path (scripts/build-ios.sh, work/deps-*);
        # no OpenGL.framework (ANGLE provides GL ES, above).
        if(NOT SDL2_INCLUDE_DIR OR NOT SDL2_LIBRARY)
            message(FATAL_ERROR "GE_IOS needs -DSDL2_INCLUDE_DIR and -DSDL2_LIBRARY (scripts/build-ios.sh)")
        endif()
        target_include_directories(ge PRIVATE ${SDL2_INCLUDE_DIR} ${SDL2_INCLUDE_DIR}/..)
        target_link_libraries(ge ${SDL2_LIBRARY})
    elseif(APPLE)
        # Homebrew SDL2 (today sdl2-compat over SDL3) + OpenGL.framework.
        set(GE_SDL2_PREFIX "/opt/homebrew" CACHE PATH "SDL2 prefix (Homebrew)")
        target_include_directories(ge PRIVATE ${GE_SDL2_PREFIX}/include ${GE_SDL2_PREFIX}/include/SDL2)
        target_link_libraries(ge ${GE_SDL2_PREFIX}/lib/libSDL2.dylib "-framework OpenGL")
    else()
        # 32-bit SDL2: needs libsdl2-dev:i386 (+ libgl-dev:i386), or set
        # SDL2_LINUX_ROOT to a self-built prefix (e.g. CI's cached, user-writable
        # install, avoiding a system-wide /usr install).
        if(SDL2_LINUX_ROOT)
            target_include_directories(ge PRIVATE ${SDL2_LINUX_ROOT}/include/SDL2)
            target_link_libraries(ge ${SDL2_LINUX_ROOT}/lib/i386-linux-gnu/libSDL2.so GL dl pthread)
            set_target_properties(ge PROPERTIES BUILD_RPATH ${SDL2_LINUX_ROOT}/lib/i386-linux-gnu)
        else()
            target_include_directories(ge PRIVATE /usr/include/SDL2)
            target_link_libraries(ge SDL2 GL dl pthread)
        endif()
    endif()
    # the files a release ships beside the game, so a fresh build runs the
    # same: controller mappings and layouts (without them a new install has
    # no default binds), the ini help and the base shader list. On iOS the app
    # bundle carries them instead (round 9b; docs/port-ios/16).
    if(NOT GE_IOS)
    add_custom_command(TARGET ge POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/tools/gamecontrollerdb.txt
            ${CMAKE_SOURCE_DIR}/tools/gamecontrollerdefault.txt
            ${CMAKE_SOURCE_DIR}/tools/midas_ini_help.txt
            $<TARGET_FILE_DIR:ge>
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:ge>/Data
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/tools/shaders_base.txt $<TARGET_FILE_DIR:ge>/Data/shaders_base.txt)
    endif()
endif()
