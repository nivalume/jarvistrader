//! The port of `tests/cpp/test_core.cpp`: the same cases, the same known-answer vectors, the same
//! property tests, and the zero-allocation gate.

use kernel_core::clock::{FiredTimer, ReplayClock, TimerHandle, TimerKey, TimerQueue};
use kernel_core::crc32c::{crc32c, crc32c_bytewise, crc32c_extend, crc32c_finish, crc32c_init};
use kernel_core::int_math::{mul_div_i64, mul_div_u64, mul_div_u64_up};
use kernel_core::rng::{mix64, philox4x32_10, CounterRng};
use kernel_core::sha256::{hex, Sha256};
use kernel_core::time::{
    civil_from_days, days_from_civil, format_rfc3339, parse_rfc3339, RFC3339_MAX_LENGTH,
};
use kernel_core::{
    load_state, save_state, DurationNanos, EventKey, FixedString, FixedVec, PriorityQueue, SlotMap,
    Status, UnixNanos,
};
use jarvis_testkit::{for_all, AllocationScope};

jarvis_testkit::install_counting_allocator!();

fn rfc3339(ns: u64) -> String {
    UnixNanos::new(ns).to_rfc3339().as_str().to_string()
}

fn hex_str(digest: &[u8; 32]) -> String {
    String::from_utf8(hex(digest).to_vec()).expect("ascii")
}

// ---- unit -------------------------------------------------------------------------------------

#[test]
fn status_names_are_stable() {
    assert_eq!(Status::CapacityExceeded.as_str(), "CapacityExceeded");
    assert_eq!(Status::InvalidArgument.code(), 1);
    assert_eq!(Status::WouldBlock.code(), 17);
    assert_eq!(format!("{}", Status::Overflow), "Overflow");
}

#[test]
fn fixed_string_enforces_capacity_and_compares_by_content() {
    let a = FixedString::<4>::from_text("abcd").expect("fits");
    assert_eq!(a.as_str(), "abcd");
    assert_eq!(FixedString::<4>::from_text("abcde"), Err(Status::OutOfRange));
    let b = FixedString::<4>::from_text("abc").expect("fits");
    assert!(b < a);
    assert_ne!(b, a);
    assert!(FixedString::<4>::default().is_empty());
}

#[test]
fn fixed_vec_refuses_to_grow_past_its_capacity() {
    let mut v: FixedVec<i32> = FixedVec::with_capacity(2);
    assert_eq!(v.push(1), Ok(()));
    assert_eq!(v.push(2), Ok(()));
    assert_eq!(v.push(3), Err(Status::CapacityExceeded));
    assert_eq!(v.len(), 2);
    assert_eq!(v.swap_remove(0), Ok(1));
    assert_eq!(v[0], 2);
    assert_eq!(v.swap_remove(5), Err(Status::OutOfRange));
    let copy = v.clone();
    assert_eq!(copy.capacity(), 2);
    assert_eq!(copy.as_slice(), &[2]);
}

#[test]
fn slot_map_handles_detect_reuse_of_a_slot() {
    let mut map: SlotMap<i32> = SlotMap::with_capacity(2);
    let h1 = map.insert(10).expect("room");
    let h2 = map.insert(20).expect("room");
    assert_eq!(map.insert(30), Err(Status::CapacityExceeded));
    assert!(map.is_full());
    assert_eq!(map.get(h1), Some(&10));
    assert_eq!(map.remove(h1), Ok(10));
    assert_eq!(map.get(h1), None);
    assert_eq!(map.remove(h1), Err(Status::NotFound));
    let h3 = map.insert(30).expect("room");
    assert_eq!(h3.index, h1.index);
    assert_eq!(h3.generation, h1.generation + 1);
    assert_eq!(map.get(h1), None);
    assert_eq!(map[h3], 30);
    assert_eq!(map.get(h2), Some(&20));
    let seen: Vec<i32> = map.values().copied().collect();
    assert_eq!(seen, vec![30, 20]); // slot order
    *map.get_mut(h2).unwrap() += 1;
    assert_eq!(map.iter().map(|(h, v)| (h.index, *v)).collect::<Vec<_>>(), vec![(0, 30), (1, 21)]);
}

#[test]
fn mul_div_computes_exactly_through_192_bits() {
    // (2^64-1)^2 * 3 / (2^64-1) / 3 == 2^64-1: the intermediate needs more than 128 bits.
    assert_eq!(mul_div_u64(u64::MAX, u64::MAX, 3, u64::MAX), Err(Status::Overflow));
    assert_eq!(mul_div_u64(u64::MAX, u64::MAX, 1, u64::MAX), Ok(u64::MAX));
    assert_eq!(
        mul_div_u64(
            18_446_744_073_000_000_000,
            1_000_000_000,
            9_223_372_036_000_000_000,
            1_000_000_000_000_000_000
        ),
        Err(Status::Overflow)
    );
    assert_eq!(mul_div_u64(7, 1, 1, 0), Err(Status::InvalidArgument));
    assert_eq!(mul_div_u64_up(7, 1, 1, 2), Ok(4));
    assert_eq!(mul_div_u64_up(8, 1, 1, 2), Ok(4)); // exact: no extra unit
                                                   // Quotients beyond 64 bits.
    assert_eq!(mul_div_u64_up(u64::MAX, u64::MAX, 2, u64::MAX - 1), Err(Status::Overflow));
    assert_eq!(mul_div_u64_up(u64::MAX, 3, 1, 2), Err(Status::Overflow));
    assert_eq!(mul_div_i64(-7, 1, 1, 2), Ok(-3)); // toward zero
    assert_eq!(mul_div_i64(7, -1, -1, 2), Ok(3));
    assert_eq!(mul_div_i64(i64::MIN, 1, 1, 1), Ok(i64::MIN));
    assert_eq!(mul_div_i64(i64::MIN, -1, 1, 1), Err(Status::Overflow));
}

#[test]
fn crc32c_check_value() {
    assert_eq!(crc32c(b"123456789"), 0xE306_9283);
    let split = crc32c_finish(crc32c_extend(crc32c_extend(crc32c_init(), b"1234"), b"56789"));
    assert_eq!(split, 0xE306_9283);
    // Longer than one 8-byte word, with a remainder: exercises the slicing-by-8 path.
    assert_eq!(
        crc32c(b"The quick brown fox jumps over the lazy dog"),
        crc32c_finish(crc32c_bytewise(
            crc32c_init(),
            b"The quick brown fox jumps over the lazy dog"
        ))
    );
}

#[test]
fn sha256_test_vectors() {
    assert_eq!(
        hex_str(&Sha256::new().finish()),
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
    );
    assert_eq!(
        hex_str(&Sha256::digest(b"abc")),
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
    );
    assert_eq!(
        hex_str(&Sha256::digest(b"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")),
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"
    );
}

#[test]
fn philox4x32_10_matches_the_random123_known_answer_vectors() {
    assert_eq!(
        philox4x32_10([0, 0, 0, 0], [0, 0]),
        [0x6627_e8d5, 0xe169_c58d, 0xbc57_ac4c, 0x9b00_dbd8]
    );
    assert_eq!(
        philox4x32_10([0xffff_ffff; 4], [0xffff_ffff; 2]),
        [0x408f_276d, 0x41c8_3b0e, 0xa20b_c7c6, 0x6d54_51fd]
    );
    assert_eq!(
        philox4x32_10(
            [0x243f_6a88, 0x85a3_08d3, 0x1319_8a2e, 0x0370_7344],
            [0xa409_3822, 0x299f_31d0]
        ),
        [0xd16c_fe09, 0x94fd_cceb, 0x5001_e420, 0x2412_6ea1]
    );
}

#[test]
fn counter_rng_draws_depend_only_on_their_key() {
    let a = CounterRng::new(42);
    let b = CounterRng::new(42);
    assert_eq!(a.draw(7, 1, 0), b.draw(7, 1, 0));
    assert_ne!(a.draw(7, 1, 0), a.draw(7, 2, 0));
    assert_ne!(a.draw(7, 1, 0), a.draw(8, 1, 0));
    assert_ne!(a.draw(7, 1, 0), a.draw(7, 1, 1));
    assert_ne!(CounterRng::new(43).draw(7, 1, 0), a.draw(7, 1, 0));
    assert_eq!(mix64(0), 0xE220_A839_7B1D_CDAF);
    for i in 0..1000u64 {
        assert!(a.below(10, i, 0, 0) < 10);
    }
}

#[test]
fn rfc3339_formatting_matches_nautilus() {
    assert_eq!(rfc3339(0), "1970-01-01T00:00:00+00:00");
    assert_eq!(rfc3339(1_000_000_000), "1970-01-01T00:00:01+00:00");
    assert_eq!(rfc3339(1_000_000_000_000_000_000), "2001-09-09T01:46:40+00:00");
    assert_eq!(rfc3339(1_500_000_000_000_000_000), "2017-07-14T02:40:00+00:00");
    assert_eq!(rfc3339(1_500_000_000_500_000_000), "2017-07-14T02:40:00.500+00:00");
    assert_eq!(rfc3339(1_500_000_000_123_456_000), "2017-07-14T02:40:00.123456+00:00");
    assert_eq!(rfc3339(1_500_000_000_123_456_789), "2017-07-14T02:40:00.123456789+00:00");
    assert_eq!(rfc3339(1_707_577_123_456_789_000), "2024-02-10T14:58:43.456789+00:00");
}

#[test]
fn rfc3339_parsing_accepts_offsets_and_rejects_invalid_instants() {
    assert_eq!(
        parse_rfc3339("2024-02-10T14:58:43.456789Z"),
        Ok(UnixNanos::new(1_707_577_123_456_789_000))
    );
    assert_eq!(
        parse_rfc3339("2024-02-10T15:58:43.456789+01:00"),
        Ok(UnixNanos::new(1_707_577_123_456_789_000))
    );
    assert_eq!(parse_rfc3339("2024-02-30T00:00:00Z"), Err(Status::OutOfRange));
    assert_eq!(parse_rfc3339("1969-12-31T23:59:59Z"), Err(Status::OutOfRange));
    assert_eq!(parse_rfc3339("2024-02-10T14:58:43.1234567891Z"), Err(Status::PrecisionLoss));
    assert_eq!(parse_rfc3339("2024-02-10 14:58:43"), Err(Status::ParseError));
    assert_eq!(parse_rfc3339("2024-02-10T14:58:43Zjunk"), Err(Status::ParseError));
}

#[test]
fn unix_nanos_arithmetic_is_checked() {
    assert_eq!(UnixNanos::new(u64::MAX).plus(DurationNanos::new(1)), Err(Status::Overflow));
    assert_eq!(UnixNanos::new(5).since(UnixNanos::new(7)), Err(Status::OutOfRange));
    assert_eq!(UnixNanos::new(7).since(UnixNanos::new(5)), Ok(DurationNanos::new(2)));
}

#[test]
fn event_key_orders_by_ts_then_source_then_seq() {
    let a = EventKey::new(UnixNanos::new(1), 2, 9);
    let b = EventKey::new(UnixNanos::new(1), 3, 0);
    let c = EventKey::new(UnixNanos::new(2), 0, 0);
    assert!(a < b);
    assert!(b < c);
    assert!(EventKey::new(UnixNanos::new(1), 2, 8) < a);
}

#[test]
fn replay_clock_never_moves_backwards() {
    use kernel_core::Clock;
    let mut clock = ReplayClock::new(UnixNanos::new(10));
    assert_eq!(clock.advance_to(UnixNanos::new(20)), Ok(()));
    assert_eq!(clock.advance_to(UnixNanos::new(15)), Err(Status::InvalidArgument));
    assert_eq!(clock.now(), UnixNanos::new(20));
}

#[test]
fn timer_queue_fires_in_deadline_then_schedule_order() {
    let mut timers = TimerQueue::with_capacity(8);
    let none = DurationNanos::default();
    timers.schedule(UnixNanos::new(100), none, TimerKey::new(1, 1)).expect("room");
    timers.schedule(UnixNanos::new(50), none, TimerKey::new(1, 2)).expect("room");
    timers.schedule(UnixNanos::new(100), none, TimerKey::new(1, 3)).expect("room");
    assert!(timers.pop_due(UnixNanos::new(49)).is_none());
    let ids: Vec<u32> =
        std::iter::from_fn(|| timers.pop_due(UnixNanos::new(200))).map(|t| t.key.id).collect();
    assert_eq!(ids, vec![2, 1, 3]);
    assert_eq!(timers.active(), 0);
}

#[test]
fn timer_queue_rearms_periodic_timers_and_honors_cancel() {
    let mut timers = TimerQueue::with_capacity(4);
    let periodic =
        timers.schedule(UnixNanos::new(10), DurationNanos::new(10), TimerKey::new(0, 7)).unwrap();
    let once =
        timers.schedule(UnixNanos::new(15), DurationNanos::default(), TimerKey::new(0, 8)).unwrap();
    assert_eq!(timers.cancel(once), Ok(()));
    let mut deadlines = Vec::new();
    while let Some(fired) = timers.pop_due(UnixNanos::new(35)) {
        assert_eq!(fired.key.id, 7);
        deadlines.push(fired.deadline.value());
    }
    assert_eq!(deadlines, vec![10, 20, 30]);
    assert_eq!(timers.next_deadline(), Some(UnixNanos::new(40)));
    assert_eq!(timers.cancel(periodic), Ok(()));
    assert_eq!(timers.next_deadline(), None);
}

#[test]
fn state_encoding_is_the_cpp_layout() {
    // FixedVec<u16>: capacity u32, length u32, items little-endian.
    let mut v: FixedVec<u16> = FixedVec::with_capacity(3);
    v.push(0x0102).unwrap();
    v.push(0xFFEE).unwrap();
    assert_eq!(save_state(&v).unwrap(), [3, 0, 0, 0, 2, 0, 0, 0, 0x02, 0x01, 0xEE, 0xFF]);
    // Round trip into a vector of the same capacity, and into one built empty.
    let mut same: FixedVec<u16> = FixedVec::with_capacity(3);
    load_state(&mut same, &save_state(&v).unwrap()).unwrap();
    assert_eq!(same.as_slice(), v.as_slice());
    let mut empty: FixedVec<u16> = FixedVec::with_capacity(0);
    load_state(&mut empty, &save_state(&v).unwrap()).unwrap();
    assert_eq!(empty.capacity(), 3);
    assert_eq!(empty.as_slice(), v.as_slice());
    let mut smaller: FixedVec<u16> = FixedVec::with_capacity(2);
    assert_eq!(load_state(&mut smaller, &save_state(&v).unwrap()), Err(Status::CapacityExceeded));
    // Option, bool, i128, EventKey.
    let key = (Some(true), (-1i128, EventKey::new(UnixNanos::new(7), 2, 3)));
    let bytes = save_state(&key).unwrap();
    assert_eq!(bytes.len(), 1 + 1 + 16 + 8 + 2 + 8);
    let mut back = (None::<bool>, (0i128, EventKey::default()));
    load_state(&mut back, &bytes).unwrap();
    assert_eq!(back, key);
    // Truncated input and trailing bytes are errors.
    let mut t = (None::<bool>, (0i128, EventKey::default()));
    assert_eq!(load_state(&mut t, &bytes[..5]), Err(Status::Truncated));
    let mut longer = bytes.clone();
    longer.push(0);
    assert_eq!(load_state(&mut t, &longer), Err(Status::InvalidArgument));
    let mut b = false;
    assert_eq!(load_state(&mut b, &[2]), Err(Status::InvalidArgument));
}

#[test]
fn slot_map_and_timer_queue_state_round_trips() {
    let mut map: SlotMap<u64> = SlotMap::with_capacity(4);
    let h0 = map.insert(5).unwrap();
    let _h1 = map.insert(6).unwrap();
    map.remove(h0).unwrap();
    let h2 = map.insert(7).unwrap();
    let bytes = save_state(&map).unwrap();
    let mut restored: SlotMap<u64> = SlotMap::with_capacity(4);
    load_state(&mut restored, &bytes).unwrap();
    assert_eq!(restored.get(h2), Some(&7));
    assert_eq!(restored.get(h0), None);
    assert_eq!(
        restored.insert(8).unwrap(),
        map.insert(8).unwrap(),
        "the free list order is restored"
    );
    let mut other: SlotMap<u64> = SlotMap::with_capacity(5);
    assert_eq!(load_state(&mut other, &bytes), Err(Status::CapacityExceeded));
    // A free list that repeats a slot, or names an occupied one, is refused.
    let mut forged = bytes.clone();
    forged.truncate(bytes.len() - 4); // drop the last free index ...
    forged.extend_from_slice(&3u32.to_le_bytes()); // ... and repeat the first
    let mut target: SlotMap<u64> = SlotMap::with_capacity(4);
    assert_eq!(load_state(&mut target, &forged), Err(Status::InvalidState));
    forged.truncate(bytes.len() - 4);
    forged.extend_from_slice(&(h2.index).to_le_bytes()); // h2's slot is occupied
    let mut target: SlotMap<u64> = SlotMap::with_capacity(4);
    assert_eq!(load_state(&mut target, &forged), Err(Status::InvalidState));

    let mut timers = TimerQueue::with_capacity(4);
    let _ =
        timers.schedule(UnixNanos::new(10), DurationNanos::new(5), TimerKey::new(1, 2)).unwrap();
    let cancelled =
        timers.schedule(UnixNanos::new(3), DurationNanos::default(), TimerKey::new(1, 3)).unwrap();
    timers.cancel(cancelled).unwrap();
    let bytes = save_state(&timers).unwrap();
    let mut restored = TimerQueue::with_capacity(4);
    load_state(&mut restored, &bytes).unwrap();
    assert_eq!(restored.peek(), timers.peek());
    assert_eq!(save_state(&restored).unwrap(), bytes);
    assert_eq!(restored.pop_due(UnixNanos::new(10)), timers.pop_due(UnixNanos::new(10)));
}

// ---- property ---------------------------------------------------------------------------------

#[test]
fn mul_div_agrees_with_128_bit_arithmetic_where_that_suffices() {
    for_all(|gen| {
        let a = gen.range_u(0, u64::from(u32::MAX));
        let b = gen.range_u(0, u64::from(u32::MAX));
        let c = gen.range_u(0, u64::from(u32::MAX));
        let d = gen.range_u(1, u64::MAX);
        let expected = u128::from(a) * u128::from(b) * u128::from(c) / u128::from(d);
        let out = mul_div_u64(a, b, c, d);
        if expected > u128::from(u64::MAX) {
            assert_eq!(out, Err(Status::Overflow));
        } else {
            assert_eq!(out, Ok(expected as u64));
        }
    });
}

#[test]
fn mul_div_up_is_the_ceiling_that_128_bit_arithmetic_computes_where_that_suffices() {
    for_all(|gen| {
        let a = gen.range_u(0, u64::from(u32::MAX));
        let b = gen.range_u(0, u64::from(u32::MAX));
        let c = gen.range_u(0, u64::from(u32::MAX));
        let d = gen.range_u(1, u64::from(u32::MAX));
        let product = u128::from(a) * u128::from(b) * u128::from(c);
        let expected = product / u128::from(d) + u128::from(product % u128::from(d) != 0);
        let out = mul_div_u64_up(a, b, c, d);
        if expected > u128::from(u64::MAX) {
            assert_eq!(out, Err(Status::Overflow));
        } else {
            assert_eq!(out, Ok(expected as u64));
        }
    });
}

#[test]
fn mul_div_by_a_divisor_that_cancels_a_factor_is_exact() {
    for_all(|gen| {
        let a = gen.next_u64();
        let b = gen.range_u(1, u64::MAX);
        assert_eq!(mul_div_u64(a, b, 1, b), Ok(a));
    });
}

#[test]
fn rfc3339_round_trips_every_instant() {
    for_all(|gen| {
        // Up to year 2262 keeps the four-digit year and the full u64 range comfortably.
        let ns = gen.range_u(0, 9_000_000_000_000_000_000);
        assert_eq!(parse_rfc3339(&rfc3339(ns)), Ok(UnixNanos::new(ns)));
    });
}

#[test]
fn civil_date_conversion_round_trips() {
    for_all(|gen| {
        let days = gen.range(-1_000_000, 1_000_000);
        let date = civil_from_days(days);
        assert_eq!(days_from_civil(date.year, date.month, date.day), days);
    });
}

#[test]
fn priority_queue_pops_keys_in_strictly_increasing_order() {
    for_all(|gen| {
        let mut queue: PriorityQueue<u32> = PriorityQueue::with_capacity(64);
        let count = gen.range_u(0, 64) as u32;
        for i in 0..count {
            let key = EventKey::new(
                UnixNanos::new(gen.range_u(0, 5)),
                gen.range_u(0, 3) as u16,
                u64::from(i),
            );
            queue.push(key, i).expect("room");
        }
        let mut previous: Option<EventKey> = None;
        let mut popped = 0;
        while let Some(entry) = queue.pop() {
            if let Some(p) = previous {
                assert!(p < entry.key);
            }
            previous = Some(entry.key);
            popped += 1;
        }
        assert_eq!(popped, count);
    });
}

#[test]
fn timer_queue_keeps_firing_correctly_under_heavy_cancellation() {
    for_all(|gen| {
        const CAPACITY: u32 = 8;
        let mut timers = TimerQueue::with_capacity(CAPACITY);
        let mut live: Vec<TimerHandle> = Vec::new();
        for step in 0..200u32 {
            if live.len() < CAPACITY as usize && gen.chance(2, 3) {
                let h = timers
                    .schedule(
                        UnixNanos::new(gen.range_u(0, 1000)),
                        DurationNanos::default(),
                        TimerKey::new(0, step),
                    )
                    .expect("room");
                live.push(h);
            } else if !live.is_empty() {
                let index = gen.below(live.len() as u64) as usize;
                assert_eq!(timers.cancel(live[index]), Ok(()));
                live.swap_remove(index);
            }
        }
        let expected_fires = live.len();
        let mut fires = 0;
        let mut last = 0;
        while let Some(fired) = timers.pop_due(UnixNanos::new(1000)) {
            assert!(fired.deadline.value() >= last);
            last = fired.deadline.value();
            fires += 1;
        }
        assert_eq!(fires, expected_fires);
    });
}

#[test]
fn timer_queue_peek_names_the_timer_that_fires_next_and_leaves_the_queue_unchanged() {
    for_all(|gen| {
        const CAPACITY: u32 = 8;
        let mut timers = TimerQueue::with_capacity(CAPACITY);
        let mut live: Vec<TimerHandle> = Vec::new();
        let mut now = 0u64;
        for step in 0..300u32 {
            let pick = gen.below(3);
            if pick == 0 && live.len() < CAPACITY as usize {
                let h = timers
                    .schedule(
                        UnixNanos::new(now + gen.range_u(0, 200)),
                        DurationNanos::default(),
                        TimerKey::new(0, step),
                    )
                    .expect("room");
                live.push(h);
            } else if pick == 1 && !live.is_empty() {
                let index = gen.below(live.len() as u64) as usize;
                assert_eq!(timers.cancel(live[index]), Ok(()));
                live.swap_remove(index);
            } else {
                // What peek says against what pop_due then fires, with the state bytes unchanged by
                // the peek (stale entries included).
                let before = save_state(&timers).unwrap();
                let peeked: Option<FiredTimer> = timers.peek();
                let after = save_state(&timers).unwrap();
                assert_eq!(before, after);
                assert_eq!(peeked.is_some(), !live.is_empty());
                if let Some(peeked) = peeked {
                    now = peeked.deadline.value();
                    let fired = timers.pop_due(UnixNanos::new(now)).expect("due");
                    assert_eq!(fired, peeked);
                    live.retain(|h| *h != fired.handle);
                }
            }
        }
    });
}

#[test]
fn sliced_and_bytewise_crc32c_agree() {
    for_all(|gen| {
        let len = gen.below(300) as usize;
        let data: Vec<u8> = (0..len).map(|_| gen.below(256) as u8).collect();
        let seed = gen.next_u64() as u32;
        assert_eq!(crc32c_extend(seed, &data), crc32c_bytewise(seed, &data));
    });
}

// ---- zero-alloc -------------------------------------------------------------------------------

#[test]
fn queue_timer_arena_and_formatting_operations_do_not_allocate() {
    let mut queue: PriorityQueue<u64> = PriorityQueue::with_capacity(256);
    let mut timers = TimerQueue::with_capacity(64);
    let mut map: SlotMap<u64> = SlotMap::with_capacity(64);
    let mut buffer = [0u8; RFC3339_MAX_LENGTH];
    let mut sink = 0u64;

    let scope = AllocationScope::new();
    for i in 0..256u64 {
        let _ = queue.push(EventKey::new(UnixNanos::new(i % 7), 0, i), i);
    }
    while let Some(entry) = queue.pop() {
        sink += entry.payload;
    }
    for i in 0..64u32 {
        let _ = timers.schedule(
            UnixNanos::new(u64::from(i)),
            DurationNanos::new(3),
            TimerKey::new(0, i),
        );
        if let Ok(h) = map.insert(u64::from(i)) {
            let _ = map.remove(h);
        }
    }
    let mut n = 0;
    while n < 500 {
        let Some(fired) = timers.pop_due(UnixNanos::new(100)) else { break };
        sink += fired.deadline.value();
        n += 1;
    }
    sink += format_rfc3339(UnixNanos::new(sink), &mut buffer).unwrap_or(0) as u64;
    let counted = scope.allocations();
    assert_eq!(counted, 0);
    assert!(sink > 0);
}
