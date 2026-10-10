//! Replaying a run log (docs/architecture.md section 5.2): every input record is stepped as it
//! was recorded (timers, batch ends and lifecycle transitions included, nothing synthesised), and
//! the outputs the engine produces are compared with the output records that followed the input
//! in the log. The first difference is a `ReplayDivergence`.

use alloc::vec::Vec;

use engine::Engine;
use kernel_core::{EventKey, Result, Status};
use model::log::{LogReader, RecordBody};
use model::outputs::Output;

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct ReplayReport {
    pub inputs: u64,
    pub outputs: u64,
}

/// Where a replay diverged from the log.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ReplayDivergence {
    /// The input whose step produced different outputs.
    pub key: EventKey,
    pub expected: Vec<Output>,
    pub actual: Vec<Output>,
}

#[derive(Debug)]
pub enum ReplayError {
    Log(Status),
    Step(Status),
    Divergence(ReplayDivergence),
}

/// Replays `log` through `engine`, which must be built from the run's configuration and
/// strategies.
pub fn replay(log: &[u8], engine: &mut Engine) -> core::result::Result<ReplayReport, ReplayError> {
    let (_, mut reader) = LogReader::open(log).map_err(ReplayError::Log)?;
    let mut report = ReplayReport::default();
    let mut pending: Option<(EventKey, Vec<Output>)> = None;
    let mut expected: Vec<Output> = Vec::new();
    loop {
        let record = reader.next_record().map_err(ReplayError::Log)?;
        let next_input = match &record {
            Some(r) => matches!(r.body, RecordBody::Input(_)),
            None => true,
        };
        if next_input {
            if let Some((key, actual)) = pending.take() {
                if actual != expected {
                    return Err(ReplayError::Divergence(ReplayDivergence {
                        key,
                        expected,
                        actual,
                    }));
                }
                report.outputs += expected.len() as u64;
                expected = Vec::new();
            }
        }
        let Some(record) = record else { break };
        match record.body {
            RecordBody::Input(event) => {
                engine.step(record.key, &event).map_err(ReplayError::Step)?;
                report.inputs += 1;
                pending = Some((record.key, engine.outputs().to_vec()));
            }
            RecordBody::Output(o) => expected.push(o),
        }
    }
    Ok(report)
}

/// Replays the inputs of `log` with no comparison: the engine's state afterwards is the run's.
pub fn restore(log: &[u8], engine: &mut Engine) -> Result<u64> {
    let (_, reader) = LogReader::open(log)?;
    let mut n = 0;
    for record in reader {
        let record = record?;
        if let RecordBody::Input(event) = record.body {
            engine.step(record.key, &event)?;
            n += 1;
        }
    }
    Ok(n)
}
