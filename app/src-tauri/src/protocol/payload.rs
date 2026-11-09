use super::{PayloadVisual, GENERATOR_RAMP8, GENERATOR_XORSHIFT32, GENERATOR_LFSR32};

#[derive(Clone, Debug)]
pub(super) struct PayloadPreview {
    pub(super) sequence: u32,
    pub(super) generator: u8,
    pub(super) sample: Vec<u8>,
}

// Compute preview statistics from a sampled, validated frame payload.
pub(super) fn build_payload_visual(preview: &PayloadPreview) -> PayloadVisual {
    let mut histogram = vec![0u32; 32];
    let mut bit_ones = [0u32; 8];

    for byte in &preview.sample {
        histogram[(*byte as usize) >> 3] += 1;
        for (bit, count) in bit_ones.iter_mut().enumerate() {
            *count += ((*byte >> bit) & 1) as u32;
        }
    }

    let denominator = preview.sample.len().max(1) as f64;
    let bit_ones_percent = bit_ones
        .iter()
        .map(|count| (*count as f64 / denominator) * 100.0)
        .collect();

    PayloadVisual {
        sequence: preview.sequence,
        generator: preview.generator,
        sample: preview.sample.clone(),
        histogram,
        bit_ones_percent,
    }
}

pub(super) fn sample_payload(payload: &[u8], limit: usize) -> Vec<u8> {
    if payload.len() <= limit {
        return payload.to_vec();
    }
    (0..limit)
        .map(|index| {
            let source = index * payload.len() / limit;
            payload[source]
        })
        .collect()
}

fn frame_seed(seed: u32, sequence: u32) -> u32 {
    let state = seed ^ 0x9E37_79B9u32.wrapping_mul(sequence.wrapping_add(1));
    if state == 0 { 0xA341_316C } else { state }
}

fn xorshift32(state: &mut u32) -> u32 {
    let mut x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    x
}

fn lfsr32(state: &mut u32) -> u32 {
    let lsb = *state & 1;
    *state >>= 1;
    if lsb != 0 {
        *state ^= 0x8020_0003;
    }
    if *state == 0 {
        *state = 1;
    }
    *state
}

pub(super) fn validate_payload(generator: u8, seed: u32, sequence: u32, payload: &[u8]) -> bool {
    match generator {
        GENERATOR_RAMP8 => payload
            .iter()
            .enumerate()
            .all(|(index, byte)| *byte == ((sequence as usize + index) & 0xFF) as u8),
        GENERATOR_XORSHIFT32 => validate_word_generator(payload, frame_seed(seed, sequence), xorshift32),
        GENERATOR_LFSR32 => validate_word_generator(payload, frame_seed(seed, sequence), lfsr32),
        _ => false,
    }
}

fn validate_word_generator(
    payload: &[u8],
    mut state: u32,
    next: fn(&mut u32) -> u32,
) -> bool {
    let mut offset = 0usize;
    while offset < payload.len() {
        let expected = next(&mut state).to_le_bytes();
        let count = (payload.len() - offset).min(4);
        if payload[offset..offset + count] != expected[..count] {
            return false;
        }
        offset += count;
    }
    true
}

