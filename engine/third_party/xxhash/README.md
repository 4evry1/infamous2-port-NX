# xxHash

Unmodified xxhash.h from Cyan4973/xxHash v0.8.3. BSD-2-Clause license and copyright are included in the header.

https://raw.githubusercontent.com/Cyan4973/xxHash/v0.8.3/xxhash.h
SHA256 17973c0dc49d9854ca26caa191f0e12f7a424b68858d9a78de3860d959d85e4b
Unmodified BSD-2-Clause header.

Used only by the RSX vertex/index content hash via XXH3_64bits_withSeed. The inline namespaced API adds no runtime library or allocation. Content hashes are ephemeral, never serialized. This replaces a custom hash with reproducible paired-bit collisions; no cryptographic security is claimed.
