#!/bin/sh

#format test results for pasting into google docs to view the pretty graphs

for (( i = 1; i <= 100; ++i )); do
    awk "NR==$i {printf \"%3u\t%8u\t%8u\t\",\$1,\$2,\$3; exit}" data
    awk "NR==$i {printf \"%3u\t%8u\t%8u\t\",\$1,\$2,\$3; exit}" data_ascend
    awk "NR==$i {printf \"%3u\t%8u\t%8u\t\",\$1,\$2,\$3; exit}" data_descend
    awk "NR==$i {printf \"%3u\t%8u\t%8u\t\",\$1,\$2,\$3; exit}" data_flat
    awk "NR==$i {printf \"%3u\t%8u\t%8u\t\",\$1,\$2,\$3; exit}" data_sparse
    awk "NR==$i {printf \"%3u\t%8u\t%8u\t\",\$1,\$2,\$3; exit}" data_zero
    echo ""
done
