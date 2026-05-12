python3 scripts/stitch_ship.py \
    -g res_unpack/content/GameParams.data \
    -d res_unpack \
    -s $1 \
    -o out/$1.glb \
    --textures \
    --with-turrets \
    --assets-bin res_unpack/content/assets.bin
