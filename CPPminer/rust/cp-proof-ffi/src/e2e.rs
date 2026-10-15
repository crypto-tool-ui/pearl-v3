use aes_gcm::{aead::Aead, Aes256Gcm, KeyInit, Nonce};
use hkdf::Hkdf;
use rand_core::OsRng;
use sha2::Sha256;
use std::{ffi::c_void, sync::Mutex};
use x25519_dalek::{PublicKey, StaticSecret};
use zeroize::Zeroizing;

use crate::write_err;

enum ClientHandshakeState {
    Legacy(StaticSecret),
    Prl7(pearl_net_crypto::ClientHandshake),
}

struct LegacyDirection {
    key: Zeroizing<[u8; 32]>,
    counter: u64,
}

struct LegacySession {
    tx: LegacyDirection,
    rx: LegacyDirection,
}

enum Session {
    Legacy(LegacySession),
    Prl7(pearl_net_crypto::SessionKeys),
}

struct SharedSession(Mutex<Session>);

fn fail(err: *mut u8, err_cap: usize, message: &str) -> i32 {
    if !err.is_null() && err_cap > 0 {
        let out = unsafe { std::slice::from_raw_parts_mut(err, err_cap) };
        write_err(Some(out), message);
    }
    -1
}

fn copy_out(bytes: &[u8], out: *mut u8, cap: usize, out_len: *mut usize) -> bool {
    if out.is_null() || out_len.is_null() || cap < bytes.len() {
        return false;
    }
    unsafe {
        std::ptr::copy_nonoverlapping(bytes.as_ptr(), out, bytes.len());
        *out_len = bytes.len();
    }
    true
}

fn derive_legacy_direction(shared: &[u8; 32], info: &[u8]) -> Result<LegacyDirection, String> {
    let mut key = Zeroizing::new([0; 32]);
    Hkdf::<Sha256>::new(Some(b"csd-pool-e2e-v1"), shared)
        .expand(info, key.as_mut())
        .map_err(|_| "legacy proxy HKDF failed".to_owned())?;
    Ok(LegacyDirection { key, counter: 0 })
}

fn legacy_encrypt(direction: &mut LegacyDirection, plaintext: &[u8]) -> Result<Vec<u8>, String> {
    let counter = direction.counter.checked_add(1).ok_or("legacy proxy counter exhausted")?;
    let mut nonce = [0; 12];
    nonce[..4].copy_from_slice(b"CSD!");
    nonce[4..].copy_from_slice(&counter.to_be_bytes());
    let cipher = Aes256Gcm::new_from_slice(direction.key.as_ref()).map_err(|_| "invalid key")?;
    let ciphertext = cipher.encrypt(Nonce::from_slice(&nonce), plaintext)
        .map_err(|_| "legacy proxy encryption failed")?;
    let mut wire = Vec::with_capacity(13 + ciphertext.len());
    wire.push(12);
    wire.extend_from_slice(&nonce);
    wire.extend_from_slice(&ciphertext);
    direction.counter = counter;
    Ok(wire)
}

fn legacy_decrypt(direction: &mut LegacyDirection, wire: &[u8]) -> Result<Vec<u8>, String> {
    if wire.len() < 29 || wire[0] != 12 || &wire[1..5] != b"CSD!" {
        return Err("invalid legacy proxy frame".into());
    }
    let counter = u64::from_be_bytes(wire[5..13].try_into().unwrap());
    if counter <= direction.counter {
        return Err("replayed legacy proxy frame".into());
    }
    let cipher = Aes256Gcm::new_from_slice(direction.key.as_ref()).map_err(|_| "invalid key")?;
    let plain = cipher.decrypt(Nonce::from_slice(&wire[1..13]), &wire[13..])
        .map_err(|_| "legacy proxy authentication failed")?;
    direction.counter = counter;
    Ok(plain)
}

#[no_mangle]
pub unsafe extern "C" fn cp_e2e_start(
    mode: i32, out: *mut u8, cap: usize, out_len: *mut usize,
    state: *mut *mut c_void, err: *mut u8, err_cap: usize,
) -> i32 {
    if state.is_null() { return fail(err, err_cap, "null handshake output"); }
    let handshake = match mode {
        1 => {
            let secret = StaticSecret::random_from_rng(OsRng);
            let public = PublicKey::from(&secret).to_bytes();
            let mut hello = hex::encode(public).into_bytes();
            hello.push(b'\n');
            if !copy_out(&hello, out, cap, out_len) { return fail(err, err_cap, "proxy hello buffer too small"); }
            ClientHandshakeState::Legacy(secret)
        }
        2 => {
            let handshake = pearl_net_crypto::ClientHandshake::random(None);
            let hello = handshake.hello();
            if !copy_out(&hello, out, cap, out_len) { return fail(err, err_cap, "proxy hello buffer too small"); }
            ClientHandshakeState::Prl7(handshake)
        }
        _ => return fail(err, err_cap, "unknown proxy encryption mode"),
    };
    *state = Box::into_raw(Box::new(handshake)).cast();
    0
}

#[no_mangle]
pub unsafe extern "C" fn cp_e2e_finish(
    state: *mut c_void, hello: *const u8, hello_len: usize,
    session: *mut *mut c_void, err: *mut u8, err_cap: usize,
) -> i32 {
    if state.is_null() || hello.is_null() || session.is_null() {
        return fail(err, err_cap, "null proxy handshake input");
    }
    let handshake = *Box::from_raw(state.cast::<ClientHandshakeState>());
    let bytes = std::slice::from_raw_parts(hello, hello_len);
    let value = match handshake {
        ClientHandshakeState::Legacy(secret) => {
            let text = match std::str::from_utf8(bytes) { Ok(v) => v.trim(), Err(_) => return fail(err, err_cap, "legacy proxy key is not UTF-8") };
            let peer: [u8; 32] = match hex::decode(text).and_then(|v| v.try_into().map_err(|_| hex::FromHexError::InvalidStringLength)) {
                Ok(v) => v,
                Err(_) => return fail(err, err_cap, "legacy proxy key must be 32-byte hex"),
            };
            let shared = secret.diffie_hellman(&PublicKey::from(peer));
            if !shared.was_contributory() { return fail(err, err_cap, "invalid legacy proxy public key"); }
            let tx = match derive_legacy_direction(shared.as_bytes(), b"aes-256-gcm-key-c2s") { Ok(v) => v, Err(e) => return fail(err, err_cap, &e) };
            let rx = match derive_legacy_direction(shared.as_bytes(), b"aes-256-gcm-key-s2c") { Ok(v) => v, Err(e) => return fail(err, err_cap, &e) };
            Session::Legacy(LegacySession { tx, rx })
        }
        ClientHandshakeState::Prl7(handshake) => match handshake.finish(bytes) {
            Ok(value) => Session::Prl7(value),
            Err(error) => return fail(err, err_cap, &format!("PRL7 proxy handshake failed: {error}")),
        },
    };
    *session = Box::into_raw(Box::new(SharedSession(Mutex::new(value)))).cast();
    0
}

#[no_mangle]
pub unsafe extern "C" fn cp_e2e_encrypt(
    session: *mut c_void, input: *const u8, input_len: usize,
    out: *mut u8, cap: usize, out_len: *mut usize, err: *mut u8, err_cap: usize,
) -> i32 {
    if session.is_null() || input.is_null() { return fail(err, err_cap, "null proxy encryption input"); }
    let plain = std::slice::from_raw_parts(input, input_len);
    let state = &*session.cast::<SharedSession>();
    let mut session = match state.0.lock() { Ok(v) => v, Err(_) => return fail(err, err_cap, "proxy session lock poisoned") };
    let result = match &mut *session {
        Session::Legacy(value) => legacy_encrypt(&mut value.tx, plain),
        Session::Prl7(value) => value.encrypt(plain).map_err(|e| e.to_string()),
    };
    match result {
        Ok(bytes) if copy_out(&bytes, out, cap, out_len) => 0,
        Ok(_) => fail(err, err_cap, "proxy encryption output buffer too small"),
        Err(e) => fail(err, err_cap, &e),
    }
}

#[no_mangle]
pub unsafe extern "C" fn cp_e2e_decrypt(
    session: *mut c_void, input: *const u8, input_len: usize,
    out: *mut u8, cap: usize, out_len: *mut usize, err: *mut u8, err_cap: usize,
) -> i32 {
    if session.is_null() || input.is_null() { return fail(err, err_cap, "null proxy decryption input"); }
    let wire = std::slice::from_raw_parts(input, input_len);
    let state = &*session.cast::<SharedSession>();
    let mut session = match state.0.lock() { Ok(v) => v, Err(_) => return fail(err, err_cap, "proxy session lock poisoned") };
    let result = match &mut *session {
        Session::Legacy(value) => legacy_decrypt(&mut value.rx, wire),
        Session::Prl7(value) => value.decrypt(wire).map_err(|e| e.to_string()),
    };
    match result {
        Ok(bytes) if copy_out(&bytes, out, cap, out_len) => 0,
        Ok(_) => fail(err, err_cap, "proxy plaintext output buffer too small"),
        Err(e) => fail(err, err_cap, &e),
    }
}

#[no_mangle]
pub unsafe extern "C" fn cp_e2e_destroy(state: *mut c_void) {
    if !state.is_null() { drop(Box::from_raw(state.cast::<SharedSession>())); }
}

#[no_mangle]
pub unsafe extern "C" fn cp_e2e_abort_handshake(state: *mut c_void) {
    if !state.is_null() { drop(Box::from_raw(state.cast::<ClientHandshakeState>())); }
}
