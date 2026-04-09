<div align="center">

# 🛰️ Satellite Change Detection
**Distributed MSE analysis for massive datasets using MPI and asynchronous prefetching**

[![Language](https://img.shields.io/badge/Language-C++17-blue?style=flat-square&logo=c%2B%2B)](https://en.cppreference.com/w/cpp/17)
[![Parallel-Framework](https://img.shields.io/badge/Framework-OpenMPI-orange?style=flat-square&logo=open-mpi)](https://www.open-mpi.org/)
[![Dependency](https://img.shields.io/badge/Library-GDAL-brightgreen?style=flat-square)](https://gdal.org/)
[![OS](https://img.shields.io/badge/OS-Linux%20/%20WSL2-lightgrey?style=flat-square&logo=linux)](https://ubuntu.com/)

[Overview](#overview) · [Strategies](#strategies) · [Benchmarks](#benchmarks) · [Installation](#installation) · [Team](#team)

</div>

---

## 📖 Overview

Processing satellite imagery at scale presents a unique challenge: **I/O Bottlenecks.** When images reach gigabyte scales, traditional sequential processing starves the CPU while it waits for the storage drive to feed it pixels.

This project implements a high-performance distributed system to calculate the **Mean Squared Error (MSE)** between temporal satellite captures. It leverages **MPI (Message Passing Interface)** to decompose workloads and utilizes **Asynchronous Prefetching** to hide disk latency.

### Key Highlights
* **Memory Safety:** Processed 3GB+ datasets on machines with limited RAM (7.6GB) using granular strip-processing.
* **Hybrid Core Optimization:** Optimized for Intel 13th Gen P/E-core architectures via dynamic load balancing.
* **I/O Masking:** Used `std::async` to fetch the next data chunk while the CPU computes the current one.

---

## 🛠️ Strategies

We implemented three distinct decomposition schemes to analyze performance trade-offs:

| Strategy | Type | Description | Best For |
| :--- | :--- | :--- | :--- |
| **Row-Wise** | Static 1D | Image is sliced into horizontal strips assigned to specific ranks. | Contiguous I/O |
| **Block-Wise** | Static 2D | Image is divided into a checkerboard grid. | Spatial Locality |
| **Pipeline** | Dynamic | **The Crown Jewel.** A Master-Worker task queue with background prefetching. | Massive Datasets |

---

## 📊 Benchmarks

Our experiments revealed a "Performance Flip" based on dataset scale.

### Normal Dataset (20-30 MB)
For small files, **Block-Static** dominates. The file fits in the L3 cache instantly, and the communication overhead of a dynamic pipeline is too high.

### Huge Dataset (1-3 GB)
For big data, the **Dynamic Pipeline** takes the crown. By overlapping I/O and computation, it masks the sluggishness of virtual disks and keeps all P-cores saturated.

| Dataset | Processors | Winner | Time (s) |
| :--- | :--- | :--- | :--- |
| Normal | 12 Cores | **Block-Static** | 0.21s |
| Huge | 8 Cores | **Pipeline-Dynamic** | 4.86s |

---

## 🚀 Installation

### Prerequisites
* Linux or WSL2
* OpenMPI
* GDAL Development Headers

```bash
# Ubuntu/Debian
sudo apt install libgdal-dev mpi-default-dev
```

### Compilation
Use the strict linker order to ensure GDAL and Pthreads are correctly mapped:
```bash
mpicxx -O3 mpi_sat_change.cpp -o mpi_sat_change -I/usr/include/gdal -lgdal -lpthread
```

### Execution
```bash
# mpirun -np [threads] ./exec [img1] [img2] [scheme_id] [granularity]
mpirun -np 8 ./mpi_sat_change original.tif copy.tif 3 100
```

This project was developed for the **Parallel & Distributed Computing (PDC)** Mid-Semester Project.
