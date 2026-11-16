//! Bounded stereo PCM packets inside the authenticated, lease-owned XR channel.
//! No timestamps are compared between headset and PC clocks.
use std::io;

pub const HEADER:usize=32;
pub const MAX_FRAMES:usize=1920;
pub const MAX_PACKET:usize=HEADER+MAX_FRAMES*4;

#[derive(Debug,Clone,Copy,PartialEq)]
pub struct Header { pub rate:u32,pub frames:u32,pub sequence:u64 }
impl Header {
    pub fn decode(bytes:&[u8])->io::Result<Self> {
        let invalid=||io::Error::new(io::ErrorKind::InvalidData,"XR PCM format/size/order");
        if bytes.len()<HEADER || &bytes[..4]!=b"NFAP" || bytes[4..8]!=[1,0,32,0] ||
           bytes[12..16]!=[2,0,1,0] {return Err(invalid());}
        let u32_at=|at|u32::from_le_bytes(bytes[at..at+4].try_into().unwrap());
        let result=Self {rate:u32_at(8),frames:u32_at(16),sequence:u64::from_le_bytes(bytes[24..32].try_into().unwrap())};
        if !(8000..=192000).contains(&result.rate) || result.frames>result.rate/100 ||
           result.frames as usize>MAX_FRAMES || (result.frames==0 && result.sequence!=0) ||
           u32_at(20)!=result.frames*4 {return Err(invalid());}
        Ok(result)
    }
    pub fn payload_len(self)->usize {self.frames as usize*4}
}

#[derive(Default)]
pub struct Reader {rate:Option<u32>,sequence:u64}
impl Reader {
    pub fn accept(&mut self,packet:&[u8])->io::Result<Header> {
        let header=Header::decode(packet)?;
        if header.sequence!=self.sequence || self.sequence==u64::MAX ||
           packet.len()!=HEADER+header.payload_len() ||
           self.rate.is_some_and(|rate|rate!=header.rate) ||
           (self.rate.is_none() && header.frames!=0) {
            return Err(io::Error::new(io::ErrorKind::InvalidData,"XR PCM stream identity/length"));
        }
        self.rate=Some(header.rate);self.sequence+=1;Ok(header)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn packet(sequence:u64,frames:u32)->Vec<u8> {
        let mut value=vec![0;32+frames as usize*4];
        value[..4].copy_from_slice(b"NFAP");value[4..8].copy_from_slice(&[1,0,32,0]);
        value[8..12].copy_from_slice(&48000u32.to_le_bytes());value[12..16].copy_from_slice(&[2,0,1,0]);
        value[16..20].copy_from_slice(&frames.to_le_bytes());value[20..24].copy_from_slice(&(frames*4).to_le_bytes());
        value[24..32].copy_from_slice(&sequence.to_le_bytes());value
    }
    #[test]fn configuration_then_bounded_audio_and_replay_rejection() {
        let mut reader=Reader::default();
        assert_eq!(reader.accept(&packet(0,0)).unwrap().rate,48000);
        assert_eq!(reader.accept(&packet(1,480)).unwrap().frames,480);
        assert!(reader.accept(&packet(1,480)).is_err());
        assert!(reader.accept(&packet(2,481)).is_err());
        assert!(reader.accept(&packet(2,0)).is_err());
        assert!(reader.accept(&packet(2,480)[..100]).is_err());
    }
    #[test]fn reject_header_before_payload_allocation_and_rate_switch() {
        let mut invalid=packet(0,0);invalid[20..24].copy_from_slice(&u32::MAX.to_le_bytes());
        assert!(Header::decode(&invalid).is_err());
        let mut reader=Reader::default();reader.accept(&packet(0,0)).unwrap();
        let mut changed=packet(1,441);changed[8..12].copy_from_slice(&44100u32.to_le_bytes());
        assert!(reader.accept(&changed).is_err());
        assert!(Reader::default().accept(&packet(0,480)).is_err());
    }
}
