#!/bin/bash

EXEC="./mpi_sat_change"
OUT_CSV="full_benchmark_results.csv"

# Strict Linker Order for WSL2
mpicxx -O3 mpi_sat_change.cpp -o mpi_sat_change -I/usr/include/gdal -lgdal -lpthread

# Test processors based on your 10-core topology
PROCESSORS=(2 4 8 12 16)
SCHEMES=("1" "2" "3")
SCHEME_NAMES=("1_Row-Static" "2_Block-Static" "3_Pipeline-Dynamic")

# Granularity: 100 rows/block fits in your 24MB L3 cache
DATASETS=(
    "Normal,original.tif,copy.tif,200"
    "Huge,huge_original.tif,huge_copy.tif,100"
)

echo "Dataset,Processors,Scheme,MSE,Time_Seconds" > $OUT_CSV

for ds_info in "${DATASETS[@]}"; do
    IFS=',' read -r DS_NAME IMG1 IMG2 GRAN <<< "$ds_info"
    echo ">>> Testing Dataset: $DS_NAME"
    
    for p in "${PROCESSORS[@]}"; do
        for i in "${!SCHEMES[@]}"; do
            s_val="${SCHEMES[$i]}"
            s_name="${SCHEME_NAMES[$i]}"
            
            echo -n "    $p Procs | $s_name ... "
            # Added 2>&1 to capture any internal GDAL errors
            OUTPUT=$(mpirun --use-hwthread-cpus --oversubscribe -np $p $EXEC "$IMG1" "$IMG2" "$s_val" "$GRAN" 2>&1)
            
            TIME=$(echo "$OUTPUT" | grep -oP '(?<=TIME:).*')
            MSE=$(echo "$OUTPUT" | grep -oP '(?<=MSE:)[^,]+')

            if [ ! -z "$TIME" ]; then
                echo "$DS_NAME,$p,$s_name,$MSE,$TIME" >> $OUT_CSV
                echo "Done ($TIME s)"
            else
                echo "Failed. Check for errors."
            fi
        done
    done
done
