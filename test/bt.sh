#!/bin/bash
# build a host test: bt.sh name [extra sources...]
cd "${BT_ROOT:-$(dirname "$0")/..}/test"
S=../engine/source
n=$1; shift
gcc $XCF -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -w -DMAPLIST='"x"' -D__gamecube__ -DGEKKO -Igxv/inc -Igxv -Istub -I$S $n.c stub_av.c \
  $S/ent.c $S/collide.c $S/npc.c $S/player.c $S/physics.c $S/scene.c $S/face.c $S/vehicle.c $S/gibs.c $S/soundscape.c $S/light.c $S/fx.c $S/world.c $S/aients.c $S/relations.c $S/sentries.c $S/flyers.c $S/mines.c $S/tanks.c $S/objects.c stub_weap.c "$@" -lm -o /tmp/$n 2>&1 | grep -E "error|undefined reference" | sort -u | head -20
