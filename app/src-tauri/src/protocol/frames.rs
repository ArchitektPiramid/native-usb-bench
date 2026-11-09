use super::{
    BenchmarkConfig, PayloadVisual, HEADER_SIZE, MAX_FRAME_SIZE, MAGIC, PREVIEW_BYTES,
    PROTOCOL_MAJOR, TYPE_DATA,
};
use super::payload::{build_payload_visual, sample_payload, validate_payload, PayloadPreview};

#[derive(Debug)]
pub(crate) struct FrameParser {
    buffer: Vec<u8>,
    expected_sequence: Option<u32>,
    config: BenchmarkConfig,
    pub(crate) frames_total: u64,
    pub(crate) dropped_frames: u64,
    pub(crate) corrupt_frames: u64,
    pub(crate) discarded_bytes: u64,
    pub(crate) last_sequence: Option<u32>,
    latest_preview: Option<PayloadPreview>,
}

impl FrameParser {
    pub(crate) fn new(config: BenchmarkConfig) -> Self {
        Self {
            buffer: Vec::with_capacity((config.frame_size as usize).min(MAX_FRAME_SIZE) * 3),
            expected_sequence: None,
            config,
            frames_total: 0,
            dropped_frames: 0,
            corrupt_frames: 0,
            discarded_bytes: 0,
            last_sequence: None,
            latest_preview: None,
        }
    }

    pub(crate) fn set_config(&mut self, config: BenchmarkConfig) {
        self.config = config;
        self.reset();
    }

    pub(crate) fn config(&self) -> &BenchmarkConfig {
        &self.config
    }

    pub(crate) fn reset(&mut self) {
        self.buffer.clear();
        self.expected_sequence = None;
        self.frames_total = 0;
        self.dropped_frames = 0;
        self.corrupt_frames = 0;
        self.discarded_bytes = 0;
        self.last_sequence = None;
        self.latest_preview = None;
    }

    /// Adds raw Bulk IN bytes and returns the number of complete application
    /// frames parsed during this call.
    pub(crate) fn push(&mut self, data: &[u8]) -> u64 {
        self.buffer.extend_from_slice(data);
        let mut parsed_now = 0u64;

        loop {
            if self.buffer.len() < HEADER_SIZE {
                return parsed_now;
            }

            if self.buffer[..4] != MAGIC {
                if let Some(pos) = self.buffer.windows(4).position(|window| window == MAGIC) {
                    self.discarded_bytes += pos as u64;
                    self.buffer.drain(..pos);
                } else {
                    let keep = self.buffer.len().min(3);
                    let drop_count = self.buffer.len() - keep;
                    self.discarded_bytes += drop_count as u64;
                    self.buffer.drain(..drop_count);
                    return parsed_now;
                }

                if self.buffer.len() < HEADER_SIZE {
                    return parsed_now;
                }
            }

            let protocol_version = self.buffer[4];
            let frame_type = self.buffer[5];
            let flags = u16::from_le_bytes([self.buffer[6], self.buffer[7]]);
            let generator = (flags & 0x00FF) as u8;
            let sequence = u32::from_le_bytes([
                self.buffer[8],
                self.buffer[9],
                self.buffer[10],
                self.buffer[11],
            ]);
            let payload_len = u32::from_le_bytes([
                self.buffer[12],
                self.buffer[13],
                self.buffer[14],
                self.buffer[15],
            ]) as usize;
            let total = HEADER_SIZE.saturating_add(payload_len);

            if protocol_version != PROTOCOL_MAJOR
                || frame_type != TYPE_DATA
                || payload_len == 0
                || total > MAX_FRAME_SIZE
                || total != self.config.frame_size as usize
                || generator != self.config.generator
            {
                self.corrupt_frames += 1;
                self.discarded_bytes += 1;
                self.buffer.drain(..1);
                continue;
            }

            if self.buffer.len() < total {
                return parsed_now;
            }

            let payload = &self.buffer[HEADER_SIZE..total];
            let payload_valid = !self.config.deterministic_payload
                || validate_payload(generator, self.config.seed, sequence, payload);
            if !payload_valid {
                self.corrupt_frames += 1;
            }

            // Do not rewind sequence tracking for duplicated or reordered frames.
            // A valid forward jump represents missing application frames.
            let mut sequence_valid = true;
            if let Some(expected) = self.expected_sequence {
                if sequence != expected {
                    let gap = sequence.wrapping_sub(expected);
                    if gap < 0x8000_0000 {
                        self.dropped_frames += gap as u64;
                    } else {
                        self.corrupt_frames += 1;
                        sequence_valid = false;
                    }
                }
            }

            if sequence_valid {
                self.expected_sequence = Some(sequence.wrapping_add(1));
                self.last_sequence = Some(sequence);
            }
            // Only fully validated frames contribute to frames/s or visual previews.
            if payload_valid && sequence_valid {
                self.latest_preview = Some(PayloadPreview {
                    sequence,
                    generator,
                    sample: sample_payload(payload, PREVIEW_BYTES),
                });
                self.frames_total += 1;
                parsed_now += 1;
            }
            self.buffer.drain(..total);
        }
    }

    pub(crate) fn payload_visual(&self) -> Option<PayloadVisual> {
        self.latest_preview.as_ref().map(build_payload_visual)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::protocol::GENERATOR_RAMP8;

    fn test_config() -> BenchmarkConfig {
        BenchmarkConfig {
            generator: GENERATOR_RAMP8,
            mode: 1,
            frame_size: 64,
            seed: 0x6D2B79F5,
            deterministic_payload: true,
        }
    }

    fn make_frame(sequence: u32) -> Vec<u8> {
        let mut frame = vec![0u8; 64];
        frame[0..4].copy_from_slice(&MAGIC);
        frame[4] = PROTOCOL_MAJOR;
        frame[5] = TYPE_DATA;
        frame[8..12].copy_from_slice(&sequence.to_le_bytes());
        frame[12..16].copy_from_slice(&(48u32).to_le_bytes());
        for (i, byte) in frame[HEADER_SIZE..].iter_mut().enumerate() {
            *byte = ((sequence as usize + i) & 0xFF) as u8;
        }
        frame
    }

    #[test]
    fn fragmented_reads_and_multiple_frames() {
        let frame1 = make_frame(0);
        let frame2 = make_frame(1);
        let mut parser = FrameParser::new(test_config());
        assert_eq!(parser.push(&frame1[..7]), 0);
        assert_eq!(parser.push(&frame1[7..]), 1);
        let combined = [frame2, make_frame(2)].concat();
        assert_eq!(parser.push(&combined), 2);
        assert_eq!(parser.frames_total, 3);
        assert_eq!(parser.corrupt_frames, 0);
    }

    #[test]
    fn corrupt_payload_is_not_counted_as_valid_frame() {
        let mut parser = FrameParser::new(test_config());
        let mut broken = make_frame(0);
        broken[HEADER_SIZE + 5] ^= 0x80;
        assert_eq!(parser.push(&broken), 0);
        assert_eq!(parser.frames_total, 0);
        assert_eq!(parser.corrupt_frames, 1);
        assert!(parser.payload_visual().is_none());
        assert_eq!(parser.push(&make_frame(1)), 1);
        assert_eq!(parser.dropped_frames, 0);
    }

    #[test]
    fn duplicate_does_not_rewind_sequence_or_inflate_drop_count() {
        let mut parser = FrameParser::new(test_config());
        assert_eq!(parser.push(&make_frame(0)), 1);
        assert_eq!(parser.push(&make_frame(0)), 0);
        assert_eq!(parser.push(&make_frame(1)), 1);
        assert_eq!(parser.corrupt_frames, 1);
        assert_eq!(parser.dropped_frames, 0);
        assert_eq!(parser.frames_total, 2);
    }

    #[test]
    fn forward_gaps_and_misalignment_are_reported() {
        let mut parser = FrameParser::new(test_config());
        assert_eq!(parser.push(&[0xFE, 0xED, 0xAA]), 0);
        assert_eq!(parser.push(&make_frame(0)), 1);
        assert_eq!(parser.push(&make_frame(3)), 1);
        assert_eq!(parser.discarded_bytes, 3);
        assert_eq!(parser.dropped_frames, 2);
    }

    #[test]
    fn sequence_wraparound_does_not_create_false_drop() {
        let mut parser = FrameParser::new(test_config());
        assert_eq!(parser.push(&make_frame(u32::MAX)), 1);
        assert_eq!(parser.push(&make_frame(0)), 1);
        assert_eq!(parser.dropped_frames, 0);
    }
}
