#include <mpi.h>
#include <gdal_priv.h>
#include <iostream>
#include <vector>
#include <future>
#include <queue>
#include <string>
#include <algorithm>

struct Task {
    int x, y, w, h;
    bool terminate;
};

// Memory-efficient reader
void read_block(GDALDataset* dataset, const Task& t, std::vector<float>& buffer) {
    if (t.terminate || t.w <= 0 || t.h <= 0) return;
    buffer.resize((size_t)t.w * t.h);
    GDALRasterBand* band = dataset->GetRasterBand(1);
    CPLErr err = band->RasterIO(GF_Read, t.x, t.y, t.w, t.h, buffer.data(), t.w, t.h, GDT_Float32, 0, 0);
}

double compute_ssd(const std::vector<float>& buf1, const std::vector<float>& buf2) {
    double ssd = 0.0;
    size_t n = std::min(buf1.size(), buf2.size());
    for (size_t i = 0; i < n; ++i) {
        double diff = (double)buf1[i] - (double)buf2[i];
        ssd += diff * diff;
    }
    return ssd;
}

int main(int argc, char** argv) {
    MPI_Init(&argc, &argv);
    int rank, size;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    if (argc != 5) {
        if (rank == 0) std::cerr << "Usage: <img1> <img2> <scheme:1|2|3> <granularity>\n";
        MPI_Finalize(); return 1;
    }

    std::string file1 = argv[1], file2 = argv[2];
    int scheme = std::stoi(argv[3]);
    int gran = std::stoi(argv[4]); // Chunk size for memory safety

    GDALAllRegister();
    GDALDataset *ds1 = (GDALDataset *)GDALOpen(file1.c_str(), GA_ReadOnly);
    GDALDataset *ds2 = (GDALDataset *)GDALOpen(file2.c_str(), GA_ReadOnly);
    if (!ds1 || !ds2) { MPI_Abort(MPI_COMM_WORLD, 1); }

    int width = ds1->GetRasterXSize();
    int height = ds1->GetRasterYSize();
    double total_pixels = (double)width * height;
    double local_ssd = 0.0, global_ssd = 0.0;

    MPI_Barrier(MPI_COMM_WORLD);
    double start_time = MPI_Wtime();

    // ==========================================
    // SCHEME 1 & 2: STATIC DECOMPOSITION
    // ==========================================
    if (scheme == 1 || scheme == 2) {
        int start_x = 0, start_y = 0, my_w = width, my_h = height;

        if (scheme == 1) { // 1D Row-wise
            int rpp = height / size; int rem = height % size;
            start_y = rank * rpp + std::min(rank, rem);
            my_h = rpp + (rank < rem ? 1 : 0);
        } else { // 2D Block-wise
            int dims[2] = {0, 0}; MPI_Dims_create(size, 2, dims);
            int px = rank % dims[0], py = rank / dims[0];
            int bw = width / dims[0], bh = height / dims[1];
            start_x = px * bw; start_y = py * bh;
            my_w = (px == dims[0]-1) ? (width - start_x) : bw;
            my_h = (py == dims[1]-1) ? (height - start_y) : bh;
        }

        // Loop through assigned area in chunks (gran x gran) to save RAM
        for (int y = start_y; y < start_y + my_h; y += gran) {
            for (int x = start_x; x < start_x + my_w; x += gran) {
                int cw = std::min(gran, (start_x + my_w) - x);
                int ch = std::min(gran, (start_y + my_h) - y);
                std::vector<float> b1, b2;
                read_block(ds1, {x, y, cw, ch, false}, b1);
                read_block(ds2, {x, y, cw, ch, false}, b2);
                local_ssd += compute_ssd(b1, b2);
            }
        }
        MPI_Reduce(&local_ssd, &global_ssd, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
    } 
    // ==========================================
    // SCHEME 3: PIPELINE (TASK QUEUE)
    // ==========================================
    else if (scheme == 3) {
        const int TAG_TASK = 2, TAG_RESULT = 3;
        if (rank == 0) { // MASTER
            std::queue<Task> tasks;
            for (int y = 0; y < height; y += gran) {
                tasks.push({0, y, width, std::min(gran, height - y), false});
            }
            int active = size - 1;
            while (active > 0) {
                double res; MPI_Status stat;
                MPI_Recv(&res, 1, MPI_DOUBLE, MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &stat);
                global_ssd += res;
                if (!tasks.empty()) {
                    Task t = tasks.front(); tasks.pop();
                    MPI_Send(&t, sizeof(Task), MPI_BYTE, stat.MPI_SOURCE, TAG_TASK, MPI_COMM_WORLD);
                } else {
                    Task term = {0, 0, 0, 0, true};
                    MPI_Send(&term, sizeof(Task), MPI_BYTE, stat.MPI_SOURCE, TAG_TASK, MPI_COMM_WORLD);
                    active--;
                }
            }
        } else { // WORKER
            Task curr, next;
            std::vector<float> cb1, cb2, nb1, nb2;
            double signal = 0;
            MPI_Send(&signal, 1, MPI_DOUBLE, 0, 0, MPI_COMM_WORLD);
            MPI_Recv(&curr, sizeof(Task), MPI_BYTE, 0, TAG_TASK, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            if (!curr.terminate) { read_block(ds1, curr, cb1); read_block(ds2, curr, cb2); }
            
            while (!curr.terminate) {
                double task_ssd = compute_ssd(cb1, cb2);
                MPI_Send(&task_ssd, 1, MPI_DOUBLE, 0, TAG_RESULT, MPI_COMM_WORLD);
                MPI_Recv(&next, sizeof(Task), MPI_BYTE, 0, TAG_TASK, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
                
                if (!next.terminate) {
                    // PREFETCHING next task
                    auto f1 = std::async(std::launch::async, read_block, ds1, next, std::ref(nb1));
                    auto f2 = std::async(std::launch::async, read_block, ds2, next, std::ref(nb2));
                    f1.get(); f2.get();
                }
                curr = next; cb1 = std::move(nb1); cb2 = std::move(nb2);
            }
        }
    }

    double end_time = MPI_Wtime();
    if (rank == 0) std::cout << "MSE:" << (global_ssd / total_pixels) << ",TIME:" << (end_time - start_time) << std::endl;

    GDALClose(ds1); GDALClose(ds2);
    MPI_Finalize();
    return 0;
}
