set -u

export  MV2_R3_NOCACHE_THRESHOLD=131072
#export  MV2_R3_THRESHOLD=131072
export MV2_RNDV_PROTOCOL="RGET"
export  MV2_VBUF_SECONDARY_POOL_SIZE=1024
#export  MV2_ON_DEMAND_THRESHOLD=9000
export  MV2_ON_DEMAND_THRESHOLD=1000
#export -n MV2_USE_MSG_OPT
export  MV2_USE_MSG_OPT=0
#export MV2_PREPOST_DEPTH=8  #for reduce rc connecttion memory
#export  MV2_HANG_WHEN_ERROR=1
export TC_OFF=1
export MV2_IBA_EAGER_THRESHOLD=524288
export MV2_VBUF_TOTAL_SIZE=524288

#export MV2_R3_NOCACHE_THRESHOLD='131072'
#export MV2_R3_THRESHOLD='131072'
#export MV2_ON_DEMAND_THRESHOLD='0'
#export MV2_RNDV_PROTOCOL=${1^^}
#export MV2_SHOW_ENV_INFO='2'
#export MV2_VBUF_SECONDARY_POOL_SIZE='2048'
#export MV2_USE_MSG_OPT='0'

mkdir -p rst
bsub -n 64 -np 4 -cgsp 64 -q q_sw_expr -share_size 6000 -cross_size 1000 -b -p -J heng_s4.kron4 -I ./osu_ympi_zalltoall -f -m 1048576 2>&1 | tee rst/osu_ympi_zalltoall.txt
