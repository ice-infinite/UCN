# Cluster Resource Baseline (CLV2-M00-05, CLV2-M00.2 clean)

> Host x64 Debug, gcc. Size rows come from the clean matrix trees (tools/m00_matrix.sh).
> Max stack = largest STATIC frame in ucn_cluster.c, MEASURED PER PROFILE in a clean -fstack-usage tree (dynamic call-chain depth is not summed; M01/M02 must keep per-frame growth bounded).

| Profile | sizeof(ucn_cluster_t) | .text | .rodata | .data | .bss | max static stack (own clean tree) |
|---|---|---|---|---|---|---|
| full | 1080 | 30780 | 320 | 0 | 0 | 208 |
| lite | 1080 | 30780 | 320 | 0 | 0 | 208 |
| nano | 1080 | 30780 | 320 | 0 | 0 | 208 |
| CORE_ONLY (ucn_core only) | n/a (cluster not linked) | 137360 | 2418 | 0 | 0 | n/a |
| CORE_ONLY cluster check | libucn_cluster.a absent (Cluster OFF pays nothing) |
