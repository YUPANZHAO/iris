use std::{
    ffi::{CStr, CString, c_char, c_int, c_void},
    path::{Path, PathBuf},
};

use libloading::Library;

#[repr(C)]
pub struct IrisBridgeVideoFrame {
    pub struct_size: u32,
    pub kind: i32,
    pub call_id: *const c_char,
    pub data: *const u8,
    pub size: usize,
    pub width: i32,
    pub height: i32,
    pub stride: i32,
    pub timestamp_ms: u64,
}

pub type VideoFrameCallback =
    unsafe extern "C" fn(*const IrisBridgeVideoFrame, *mut c_void);

type CreateRuntime = unsafe extern "C" fn(
    *const c_char,
    *mut *mut c_void,
    *mut *mut c_char,
) -> c_int;
type DestroyRuntime = unsafe extern "C" fn(*mut c_void);
type RuntimeCommand = unsafe extern "C" fn(
    *mut c_void,
    *const c_char,
    *mut *mut c_char,
    *mut *mut c_char,
) -> c_int;
type PollEvents = unsafe extern "C" fn(
    *mut c_void,
    *mut *mut c_char,
    *mut *mut c_char,
) -> c_int;
type Snapshot = unsafe extern "C" fn(
    *mut c_void,
    *mut *mut c_char,
    *mut *mut c_char,
) -> c_int;
type SetVideoFrameSink = unsafe extern "C" fn(
    *mut c_void,
    Option<VideoFrameCallback>,
    *mut c_void,
    *mut *mut c_char,
) -> c_int;
type StringFree = unsafe extern "C" fn(*mut c_char);

pub struct NativeApi {
    library: Library,
}

impl NativeApi {
    pub fn load() -> Result<Self, String> {
        let path = bridge_path()?;
        if let Some(parent) = path.parent() {
            prepend_dll_search_path(parent)?;
        }
        // SAFETY: the bridge is the ABI companion built by this repository.
        let library = unsafe { Library::new(&path) }
            .map_err(|error| format!("cannot load {}: {error}", path.display()))?;
        Ok(Self { library })
    }

    pub fn create_runtime(
        &self,
        config_json: &str,
    ) -> Result<*mut c_void, String> {
        let create: CreateRuntime =
            self.symbol(b"iris_bridge_runtime_create\0")?;
        let config = CString::new(config_json)
            .map_err(|_| "runtime config contains a NUL byte".to_owned())?;
        let mut runtime = std::ptr::null_mut();
        let mut error = std::ptr::null_mut();
        // SAFETY: arguments match the C ABI in c_api.h.
        let status = unsafe { create(config.as_ptr(), &mut runtime, &mut error) };
        self.finish(status, error, runtime, "runtime initialization failed")
    }

    pub fn destroy_runtime(&self, runtime: *mut c_void) {
        if runtime.is_null() {
            return;
        }
        if let Ok(destroy) = self.symbol::<DestroyRuntime>(
            b"iris_bridge_runtime_destroy\0",
        ) {
            // SAFETY: runtime is live until the worker drops it.
            unsafe { destroy(runtime) };
        }
    }

    pub fn command(
        &self,
        runtime: *mut c_void,
        command_json: &str,
    ) -> Result<String, String> {
        let command: RuntimeCommand =
            self.symbol(b"iris_bridge_runtime_command\0")?;
        let input = CString::new(command_json)
            .map_err(|_| "command contains a NUL byte".to_owned())?;
        let mut result = std::ptr::null_mut();
        let mut error = std::ptr::null_mut();
        // SAFETY: arguments match the C ABI in c_api.h.
        let status = unsafe {
            command(runtime, input.as_ptr(), &mut result, &mut error)
        };
        let result = self.finish(
            status,
            error,
            result as *mut u8,
            "Iris command failed",
        )?;
        // SAFETY: a successful command reports a JSON string.
        let text = unsafe { CStr::from_ptr(result as *const c_char) }
            .to_string_lossy()
            .into_owned();
        self.free_string(result as *mut c_char);
        Ok(text)
    }

    pub fn poll_events(&self, runtime: *mut c_void) -> Result<String, String> {
        let poll: PollEvents =
            self.symbol(b"iris_bridge_runtime_poll_events\0")?;
        let mut result = std::ptr::null_mut();
        let mut error = std::ptr::null_mut();
        // SAFETY: arguments match the C ABI in c_api.h.
        let status = unsafe { poll(runtime, &mut result, &mut error) };
        let result = self.finish(
            status,
            error,
            result,
            "Iris event polling failed",
        )?;
        // SAFETY: successful polling reports a JSON string.
        let text =
            unsafe { CStr::from_ptr(result) }.to_string_lossy().into_owned();
        self.free_string(result);
        Ok(text)
    }

    pub fn snapshot(&self, runtime: *mut c_void) -> Result<String, String> {
        let snapshot: Snapshot =
            self.symbol(b"iris_bridge_runtime_snapshot\0")?;
        let mut result = std::ptr::null_mut();
        let mut error = std::ptr::null_mut();
        // SAFETY: arguments match the C ABI in c_api.h.
        let status = unsafe { snapshot(runtime, &mut result, &mut error) };
        let result = self.finish(
            status,
            error,
            result,
            "Iris snapshot failed",
        )?;
        // SAFETY: successful snapshots report a JSON string.
        let text =
            unsafe { CStr::from_ptr(result) }.to_string_lossy().into_owned();
        self.free_string(result);
        Ok(text)
    }

    pub fn set_video_frame_sink(
        &self,
        runtime: *mut c_void,
        callback: Option<VideoFrameCallback>,
        user_data: *mut c_void,
    ) -> Result<(), String> {
        let set: SetVideoFrameSink =
            self.symbol(b"iris_bridge_runtime_set_video_frame_sink\0")?;
        let mut error = std::ptr::null_mut();
        // SAFETY: arguments match the C ABI in c_api.h.
        let status =
            unsafe { set(runtime, callback, user_data, &mut error) };
        self.finish(status, error, (), "cannot install video frame sink")
    }

    fn finish<T>(
        &self,
        status: c_int,
        error: *mut c_char,
        success: T,
        fallback: &str,
    ) -> Result<T, String> {
        let message = if error.is_null() {
            fallback.to_owned()
        } else {
            // SAFETY: the bridge returns a NUL-terminated UTF-8 string.
            let text = unsafe { CStr::from_ptr(error) }
                .to_string_lossy()
                .into_owned();
            self.free_string(error);
            text
        };
        if status == 0 {
            Ok(success)
        } else {
            Err(message)
        }
    }

    fn free_string(&self, value: *mut c_char) {
        if value.is_null() {
            return;
        }
        if let Ok(free) =
            self.symbol::<StringFree>(b"iris_bridge_string_free\0")
        {
            // SAFETY: the pointer came from this bridge.
            unsafe { free(value) };
        }
    }

    fn symbol<T: Copy>(&self, name: &[u8]) -> Result<T, String> {
        // SAFETY: symbol names and signatures match the bridge ABI.
        unsafe { self.library.get::<T>(name) }
            .map(|symbol| *symbol)
            .map_err(|error| {
                format!(
                    "missing bridge function {}: {error}",
                    String::from_utf8_lossy(name)
                        .trim_end_matches('\0')
                )
            })
    }
}

pub struct RuntimeHandle {
    api: NativeApi,
    raw: *mut c_void,
}

impl RuntimeHandle {
    pub fn create(api: NativeApi, config_json: &str) -> Result<Self, String> {
        let raw = api.create_runtime(config_json)?;
        Ok(Self { api, raw })
    }

    pub fn raw(&self) -> *mut c_void {
        self.raw
    }

    pub fn api(&self) -> &NativeApi {
        &self.api
    }
}

impl Drop for RuntimeHandle {
    fn drop(&mut self) {
        self.api.destroy_runtime(self.raw);
    }
}

fn bridge_path() -> Result<PathBuf, String> {
    let mut candidates = Vec::new();
    if let Some(path) = std::env::var_os("IRIS_BRIDGE_DLL") {
        candidates.push(PathBuf::from(path));
    }
    if let Ok(executable) = std::env::current_exe() {
        if let Some(parent) = executable.parent() {
            candidates.push(parent.join("iris_bridge.dll"));
        }
    }
    let manifest = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    candidates.push(
        manifest
            .join("..")
            .join("..")
            .join("build")
            .join("windows")
            .join("Release")
            .join("iris_bridge.dll"),
    );
    candidates.push(
        manifest
            .join("..")
            .join("..")
            .join("build")
            .join("windows")
            .join("iris_bridge.dll"),
    );

    candidates
        .into_iter()
        .find(|path| path.is_file())
        .ok_or_else(|| {
            "iris_bridge.dll was not found; run scripts/build.ps1 first".to_owned()
        })
}

fn prepend_dll_search_path(directory: &Path) -> Result<(), String> {
    let current = std::env::var_os("PATH").unwrap_or_default();
    let mut paths = vec![directory.to_path_buf()];
    paths.extend(std::env::split_paths(&current));
    let joined = std::env::join_paths(paths)
        .map_err(|error| format!("cannot extend DLL search path: {error}"))?;
    // SAFETY: this runs before Iris media worker threads start.
    unsafe {
        std::env::set_var("PATH", joined);
    }
    Ok(())
}
