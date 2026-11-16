use std::{
    collections::HashMap,
    ffi::{CStr, c_void},
    sync::{Arc, Mutex, OnceLock},
};

use crate::iris::ffi::IrisBridgeVideoFrame;

#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub enum FrameKind {
    Remote,
    LocalPreview,
}

#[derive(Clone)]
pub struct StoredFrame {
    pub width: u32,
    pub height: u32,
    pub stride: u32,
    pub sequence: u64,
    pub data: Vec<u8>,
}

struct Observer {
    id: u64,
    kind: FrameKind,
    context: nsh_platform_windows::ComponentContext,
}

pub struct FrameStore {
    frames: Mutex<HashMap<(String, FrameKind), Arc<StoredFrame>>>,
    observers: Mutex<Vec<Observer>>,
    next_observer: Mutex<u64>,
    sequence: Mutex<u64>,
}

impl Default for FrameStore {
    fn default() -> Self {
        Self {
            frames: Mutex::new(HashMap::new()),
            observers: Mutex::new(Vec::new()),
            next_observer: Mutex::new(1),
            sequence: Mutex::new(1),
        }
    }
}

impl FrameStore {
    pub fn accept(&self, native: &IrisBridgeVideoFrame) {
        if native.width <= 0 ||
            native.height <= 0 ||
            native.stride < native.width * 4 ||
            native.data.is_null()
        {
            return;
        }
        let row_bytes = native.width as usize * 4;
        let size = row_bytes * native.height as usize;
        if native.size < size {
            return;
        }
        let call_id = if native.call_id.is_null() {
            String::new()
        } else {
            // SAFETY: the bridge keeps call_id alive for this callback.
            unsafe { CStr::from_ptr(native.call_id) }
                .to_string_lossy()
                .into_owned()
        };
        let kind = if native.kind == 1 {
            FrameKind::LocalPreview
        } else {
            FrameKind::Remote
        };
        let mut packed = Vec::with_capacity(size);
        for row in 0..native.height as usize {
            // SAFETY: size and stride were validated above.
            let source = unsafe {
                std::slice::from_raw_parts(
                    native.data.add(row * native.stride as usize),
                    row_bytes,
                )
            };
            packed.extend_from_slice(source);
        }

        let sequence = {
            let mut sequence = self
                .sequence
                .lock()
                .unwrap_or_else(|error| error.into_inner());
            let value = *sequence;
            *sequence += 1;
            value
        };
        let frame = Arc::new(StoredFrame {
            width: native.width as u32,
            height: native.height as u32,
            stride: (native.width * 4) as u32,
            sequence,
            data: packed,
        });
        self.frames
            .lock()
            .unwrap_or_else(|error| error.into_inner())
            .insert((call_id, kind), frame);

        let observers = self
            .observers
            .lock()
            .unwrap_or_else(|error| error.into_inner());
        for observer in observers.iter().filter(|observer| observer.kind == kind) {
            observer.context.redraw();
        }
    }

    pub fn frame(
        &self,
        call_id: &str,
        kind: FrameKind,
    ) -> Option<Arc<StoredFrame>> {
        let frames = self
            .frames
            .lock()
            .unwrap_or_else(|error| error.into_inner());
        frames
            .get(&(call_id.to_owned(), kind))
            .cloned()
    }

    pub fn discard_call(&self, call_id: &str) {
        self.frames
            .lock()
            .unwrap_or_else(|error| error.into_inner())
            .retain(|(id, _), _| id != call_id);
    }

    fn register(
        self: &Arc<Self>,
        kind: FrameKind,
        context: nsh_platform_windows::ComponentContext,
    ) -> u64 {
        let mut next = self
            .next_observer
            .lock()
            .unwrap_or_else(|error| error.into_inner());
        let id = *next;
        *next += 1;
        self.observers
            .lock()
            .unwrap_or_else(|error| error.into_inner())
            .push(Observer {
                id,
                kind,
                context,
            });
        id
    }

    fn unregister(&self, id: u64) {
        self.observers
            .lock()
            .unwrap_or_else(|error| error.into_inner())
            .retain(|observer| observer.id != id);
    }
}

pub fn frame_store() -> Arc<FrameStore> {
    static STORE: OnceLock<Arc<FrameStore>> = OnceLock::new();
    STORE
        .get_or_init(|| Arc::new(FrameStore::default()))
        .clone()
}

pub unsafe extern "C" fn frame_callback(
    frame: *const IrisBridgeVideoFrame,
    user_data: *mut c_void,
) {
    if frame.is_null() || user_data.is_null() {
        return;
    }
    // SAFETY: user_data is the Arc<FrameStore> kept alive by IrisClient.
    let store = unsafe { &*(user_data as *const FrameStore) };
    // SAFETY: the bridge passes a valid frame for this callback.
    store.accept(unsafe { &*frame });
}

#[cfg(windows)]
pub fn register(components: &nsh_platform_windows::Components) {
    use nsh_platform_windows::ComponentContext;
    use nsh_platform_windows::NativeComponent;
    use nsh_platform_windows::windows::Win32::Graphics::Direct2D::{
        Common::{
            D2D_RECT_F, D2D1_ALPHA_MODE_PREMULTIPLIED,
            D2D1_COLOR_F, D2D1_PIXEL_FORMAT,
        },
        D2D1_BITMAP_OPTIONS_NONE, D2D1_BITMAP_PROPERTIES1,
        D2D1_INTERPOLATION_MODE_LINEAR, ID2D1DeviceContext,
    };
    use nsh_platform_windows::windows::Win32::Graphics::Dxgi::Common::{
        DXGI_FORMAT_B8G8R8A8_UNORM,
    };
    use nsh_platform_windows::windows::Win32::Graphics::Direct2D::Common::{
        D2D_SIZE_U,
    };

    struct Video {
        store: Arc<FrameStore>,
        context: ComponentContext,
        observer: u64,
        call_id: String,
        kind: FrameKind,
    }

    impl Drop for Video {
        fn drop(&mut self) {
            self.store.unregister(self.observer);
        }
    }

    impl NativeComponent for Video {
        fn draw(
            &self,
            dc: &ID2D1DeviceContext,
            (width, height): (f32, f32),
        ) {
            let black = D2D1_COLOR_F {
                r: 0.035,
                g: 0.047,
                b: 0.067,
                a: 1.0,
            };
            // SAFETY: Direct2D calls are made inside NSH's draw callback.
            unsafe {
                if let Ok(brush) = dc.CreateSolidColorBrush(&black, None) {
                    dc.FillRectangle(
                        &D2D_RECT_F {
                            left: 0.0,
                            top: 0.0,
                            right: width,
                            bottom: height,
                        },
                        &brush,
                    );
                }
            }
            let Some(frame) = self.store.frame(&self.call_id, self.kind) else {
                return;
            };
            let properties = D2D1_BITMAP_PROPERTIES1 {
                pixelFormat: D2D1_PIXEL_FORMAT {
                    format: DXGI_FORMAT_B8G8R8A8_UNORM,
                    alphaMode: D2D1_ALPHA_MODE_PREMULTIPLIED,
                },
                dpiX: 96.0,
                dpiY: 96.0,
                bitmapOptions: D2D1_BITMAP_OPTIONS_NONE,
                colorContext: std::mem::ManuallyDrop::new(None),
            };
            let size = D2D_SIZE_U {
                width: frame.width,
                height: frame.height,
            };
            // SAFETY: frame.data contains frame.height rows of width * 4 BGRA.
            let bitmap = unsafe {
                dc.CreateBitmap(
                    size,
                    Some(frame.data.as_ptr().cast()),
                    frame.stride,
                    &properties,
                )
            };
            let Ok(bitmap) = bitmap else {
                return;
            };
            let frame_width = frame.width as f32;
            let frame_height = frame.height as f32;
            let scale = (width / frame_width).min(height / frame_height);
            let draw_width = frame_width * scale;
            let draw_height = frame_height * scale;
            let left = (width - draw_width) * 0.5;
            let top = (height - draw_height) * 0.5;
            // SAFETY: bitmap belongs to the device of this Direct2D context.
            unsafe {
                dc.DrawBitmap(
                    &bitmap,
                    Some(&D2D_RECT_F {
                        left,
                        top,
                        right: left + draw_width,
                        bottom: top + draw_height,
                    }),
                    1.0,
                    D2D1_INTERPOLATION_MODE_LINEAR,
                    None,
                    None,
                );
            }
            let _ = frame.sequence;
        }

        fn set_prop(
            &mut self,
            name: &str,
            value: Option<&nsh_core::JsonValue>,
        ) {
            if name == "callId" {
                self.call_id = value
                    .and_then(|value| value.as_str())
                    .unwrap_or_default()
                    .to_owned();
                self.context.redraw();
            }
        }
    }

    let make_factory = |name: &'static str, kind: FrameKind| {
        components.register(
            name,
            std::rc::Rc::new(move |context: ComponentContext| {
                let store = frame_store();
                let observer = store.register(kind, context.clone());
                Box::new(Video {
                    store,
                    context,
                    observer,
                    call_id: String::new(),
                    kind,
                }) as Box<dyn NativeComponent>
            }),
        );
    };
    make_factory("iris.remote-video", FrameKind::Remote);
    make_factory("iris.local-video", FrameKind::LocalPreview);
}
