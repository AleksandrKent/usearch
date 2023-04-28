import datetime
import numpy as np

import faiss
import usearch


print('Welcome to basic kANN benchmark!')


def read_fbin(filename, start_idx=0, chunk_size=None):
    """ Read *.fbin file that contains float32 vectors
    Args:
        :param filename (str): path to *.fbin file
        :param start_idx (int): start reading vectors from this index
        :param chunk_size (int): number of vectors to read. 
                                 If None, read all vectors
    Returns:
        Array of float32 vectors (numpy.ndarray)
    """
    with open(filename, "rb") as f:
        nvecs, dim = np.fromfile(f, count=2, dtype=np.int32)
        nvecs = (nvecs - start_idx) if chunk_size is None else chunk_size
        arr = np.fromfile(f, count=nvecs * dim, dtype=np.float32,
                          offset=start_idx * 4 * dim)
    return arr.reshape(nvecs, dim)


print('Will read datasets!')
xb = read_fbin('datasets/wiki_1M/base.1M.fbin')
xq = read_fbin('datasets/wiki_1M/query.public.100K.fbin')

d = 256  # vector size
M = 16
efSearch = 32  # number of entry points (neighbors) we use on each layer
efConstruction = 128  # number of entry points used on each layer
labels = np.arange(xb.shape[0], dtype=np.longlong)
print('Will benchmark algorithms!')


def measure(f) -> float:
    a = datetime.datetime.now()
    f()
    b = datetime.datetime.now()
    c = b - a
    print(f'Took: {c.seconds} seconds')
    return c.seconds


index_meta = faiss.IndexHNSWFlat(d, M)
index_meta.hnsw.efSearch = efSearch
index_meta.hnsw.efConstruction = efConstruction
dt = measure(lambda: index_meta.add(xb))
print(f'- Vectors per second: {xb.shape[0]/dt:.2f}')

index_unum = usearch.make_index(
    expansion_construction=efConstruction,
    expansion_search=efSearch,
    connectivity=M,
)
dt = measure(lambda: index_unum.add(labels, xb, copy=True))
print(f'- Vectors per second: {xb.shape[0]/dt:.2f}')
