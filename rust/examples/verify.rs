/// Verify: run 4str_0int ht=65536 lf=0.5 sel=0.1, print NumGroups + agg checksum.
use taper_hashmap::column_marshaller::{TaperColumnSerializeHandler, ColumnDesc, ColumnInput};
use rand_mt::Mt19937GenRand64;
use xxhash_rust::xxh3::xxh3_64_with_seed;

fn hash_bytes(data: &[u8], seed: u64) -> u64 { xxh3_64_with_seed(data, seed) }

fn main() {
    let num_str_cols = 4usize;
    let ht_size = 65536usize;
    let load_factor = 0.5f64;
    let selectivity = 0.1f64;
    let num_probe_rows = 1_000_000usize;
    let num_keys = (ht_size as f64 * load_factor) as usize;

    let mut rng = Mt19937GenRand64::new(42);

    // Build keys
    let build_str_cols: Vec<Vec<Vec<u8>>> = (0..num_str_cols)
        .map(|c| (0..num_keys).map(|i| format!("key_{}_c{}", i, c).into_bytes()).collect())
        .collect();
    let build_hashes: Vec<u64> = (0..num_keys)
        .map(|i| {
            let mut h = 0u64;
            for c in 0..num_str_cols { h = hash_bytes(&build_str_cols[c][i], h); }
            h
        }).collect();
    let build_values: Vec<i64> = (0..num_keys).map(|i| (i % 1000) as i64).collect();

    // Probe keys
    let num_hits = (num_probe_rows as f64 * selectivity) as usize;
    let num_misses = num_probe_rows - num_hits;

    let mut probe_str_cols: Vec<Vec<Vec<u8>>> = vec![Vec::with_capacity(num_probe_rows); num_str_cols];
    let mut probe_hashes: Vec<u64> = Vec::with_capacity(num_probe_rows);

    for _ in 0..num_hits {
        let idx = (rng.next_u64() as usize) % num_keys;
        for c in 0..num_str_cols { probe_str_cols[c].push(build_str_cols[c][idx].clone()); }
        probe_hashes.push(build_hashes[idx]);
    }
    for i in 0..num_misses {
        let mut h = 0u64;
        for c in 0..num_str_cols {
            let s = format!("miss_{}_{}", i, c).into_bytes();
            h = hash_bytes(&s, h);
            probe_str_cols[c].push(s);
        }
        probe_hashes.push(h);
    }

    // Shuffle
    let mut order: Vec<usize> = (0..num_probe_rows).collect();
    for i in (1..num_probe_rows).rev() { order.swap(i, (rng.next_u64() as usize) % (i + 1)); }
    let probe_str_cols: Vec<Vec<Vec<u8>>> = (0..num_str_cols)
        .map(|c| order.iter().map(|&i| probe_str_cols[c][i].clone()).collect()).collect();
    let probe_hashes: Vec<u64> = order.iter().map(|&i| probe_hashes[i]).collect();
    let probe_values: Vec<i64> = (0..num_probe_rows).map(|i| (i % 1000) as i64).collect();

    // Combine
    let mut all_str_cols = build_str_cols;
    for c in 0..num_str_cols { all_str_cols[c].extend(probe_str_cols[c].iter().cloned()); }
    let mut all_hashes: Vec<u64> = build_hashes; all_hashes.extend_from_slice(&probe_hashes);
    let mut all_values: Vec<i64> = build_values; all_values.extend_from_slice(&probe_values);

    let total_rows = all_hashes.len();

    // Run taper
    let num_misses_actual = num_probe_rows - num_hits;
    let distinct_keys = num_keys + num_misses_actual;
    let min_slots = (distinct_keys as f64 / 0.85) as usize;
    let num_chunks = ((min_slots + 7) / 8).next_power_of_two();

    let mut col_descs: Vec<ColumnDesc> = Vec::new();
    for _ in 0..num_str_cols { col_descs.push(ColumnDesc::Varchar); }

    let mut table = TaperColumnSerializeHandler::new(&col_descs, 8, num_chunks);

    const BATCH_SIZE: usize = 410;
    let num_batches = (total_rows + BATCH_SIZE - 1) / BATCH_SIZE;

    for batch_idx in 0..num_batches {
        let start = batch_idx * BATCH_SIZE;
        let end = (start + BATCH_SIZE).min(total_rows);
        let batch_hashes = &all_hashes[start..end];
        let batch_values = &all_values[start..end];
        let str_slices: Vec<Vec<&[u8]>> = (0..num_str_cols)
            .map(|c| all_str_cols[c][start..end].iter().map(|s| s.as_slice()).collect())
            .collect();
        let mut columns: Vec<ColumnInput> = Vec::new();
        for c in 0..num_str_cols { columns.push(ColumnInput::Varchar(&str_slices[c])); }
        table.emplace_table_with_decode(batch_hashes, &columns, batch_values);
    }

    println!("Rust: NumGroups = {}, AggChecksum = {}", table.num_groups(), table.agg_checksum());
}
