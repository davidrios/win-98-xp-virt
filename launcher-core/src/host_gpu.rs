//! Can this host run the Direct3D executor, and how well? (ADR-013)
//!
//! The paravirtual D3D device's executor is DXVK (ADR-007), and DXVK 3.1
//! wants **Vulkan 1.3**: `DxvkVulkanApiVersion = VK_API_VERSION_1_3` goes
//! into `VkApplicationInfo::apiVersion` at instance creation, which a
//! pre-1.3 loader answers with `ERROR_INCOMPATIBLE_DRIVER`, and every
//! adapter whose `properties.apiVersion` is below it returns early out of
//! `DxvkDeviceCapabilities` with no capabilities at all. A host that
//! fails either check boots machines normally and just has no 3D —
//! QEMU's `d3dpt_exec_load.c` says "no executor" into a log nobody reads.
//! This is the launcher asking the same question first, so the answer can
//! be a sentence in a window instead of an absence.
//!
//! **A software Vulkan driver counts.** lavapipe answers 1.3 on any CPU,
//! and DXVK ranks a `CPU` device last but never excludes it, so the
//! executor does run there — badly, since a software rasteriser competes
//! for the host CPU that TCG is already using for the guest. That is a
//! thing to warn about, not a thing to refuse on the user's behalf: the
//! verdict is "available", and [`HostGpu::is_slow`] is what puts the
//! warning next to it.
//!
//! Two other details, both deliberate:
//!
//! * The loader is opened **dynamically** (`Entry::load`). A box with no
//!   `libvulkan` at all is a report, not a failed start — and the
//!   launcher must run on exactly those boxes to explain itself.
//! * `VK_KHR_portability_enumeration` is opted into when the loader
//!   offers it, the same opt-in DXVK makes. Without it a Vulkan-on-Metal
//!   driver (KosmicKrisp, MoltenVK) is invisible and a Mac would be told
//!   it has no Vulkan when it has one.
//!
//! What we do *not* do is decide anything for the machine: a host below
//! the bar still runs every guest, through the OpenGL pass-through with
//! WineD3D in the guest (doc 04). The probe picks the sentence, not the
//! stack.

use ash::vk;
use std::sync::OnceLock;

/// What the host can offer the D3D device, worst to best.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum HostGpu {
    /// No Vulkan loader on the box (no `libvulkan`, or it would not load).
    NoLoader,
    /// A loader, but it answers below 1.3 — DXVK's `vkCreateInstance`
    /// would fail outright with `ERROR_INCOMPATIBLE_DRIVER`.
    LoaderTooOld,
    /// A 1.3 loader that enumerates nothing: no ICD, or every driver
    /// filtered out.
    NoDevice,
    /// Devices, but none of them reaches 1.3. DXVK's own words for this
    /// are "No adapters found … A Vulkan 1.3 capable setup is required."
    DeviceTooOld,
    /// The only device at 1.3 is a software rasteriser. The executor
    /// runs; it will be very slow.
    SoftwareOnly,
    /// A hardware device at 1.3 or newer: the executor will run.
    Accelerated,
}

impl HostGpu {
    /// Whether the paravirtual Direct3D device will work at all. True for
    /// a software driver too — see [`is_slow`](Self::is_slow), which is
    /// the difference the caller should show.
    pub fn d3d_available(self) -> bool {
        matches!(self, HostGpu::Accelerated | HostGpu::SoftwareOnly)
    }

    /// Available, but on a software rasteriser: worth a warning next to
    /// the verdict rather than a refusal in place of it.
    pub fn is_slow(self) -> bool {
        self == HostGpu::SoftwareOnly
    }

    /// One line for a window, in the second person, saying what this host
    /// does rather than what it lacks.
    pub fn headline(self) -> &'static str {
        match self {
            HostGpu::Accelerated => "Direct3D pass-through available (Vulkan 1.3 hardware).",
            HostGpu::SoftwareOnly => {
                "Direct3D pass-through will run on a software Vulkan driver: expect it to be very slow."
            }
            HostGpu::DeviceTooOld => {
                "This GPU is below Vulkan 1.3; 3D goes through OpenGL instead."
            }
            HostGpu::NoDevice => "No Vulkan device was found; 3D goes through OpenGL instead.",
            HostGpu::LoaderTooOld => {
                "This host's Vulkan is older than 1.3; 3D goes through OpenGL instead."
            }
            HostGpu::NoLoader => "No Vulkan on this host; 3D goes through OpenGL instead.",
        }
    }

    /// The second line: where the other 3D path is, whether it is the
    /// only one left or merely the faster one here. `None` when the host
    /// has a real GPU and there is nothing to say.
    pub fn advice(self) -> Option<&'static str> {
        match self {
            HostGpu::Accelerated => None,
            HostGpu::SoftwareOnly => Some(
                "A game rendering in software may well be faster through the guest tools' \
                 WineD3D set (SETUP /GAME on the guest-tools disc) — worth trying both.",
            ),
            _ => Some(
                "Install the guest tools' WineD3D set next to the game \
                 (SETUP /GAME on the guest-tools disc).",
            ),
        }
    }
}

/// One physical device as the loader reports it.
#[derive(Debug, Clone)]
pub struct Device {
    pub name: String,
    pub kind: vk::PhysicalDeviceType,
    /// `apiVersion` as (major, minor, patch).
    pub api: (u32, u32, u32),
}

impl Device {
    /// Vulkan 1.3 or newer, which is what DXVK checks per adapter.
    pub fn meets_bar(&self) -> bool {
        (self.api.0, self.api.1) >= (1, 3)
    }

    pub fn is_software(&self) -> bool {
        self.kind == vk::PhysicalDeviceType::CPU
    }

    pub fn kind_name(&self) -> &'static str {
        match self.kind {
            vk::PhysicalDeviceType::DISCRETE_GPU => "discrete GPU",
            vk::PhysicalDeviceType::INTEGRATED_GPU => "integrated GPU",
            vk::PhysicalDeviceType::VIRTUAL_GPU => "virtual GPU",
            vk::PhysicalDeviceType::CPU => "software",
            _ => "other",
        }
    }
}

/// The whole answer: the verdict, and the evidence behind it.
#[derive(Debug, Clone)]
pub struct Probe {
    pub gpu: HostGpu,
    /// The loader's own version, `None` when there is no loader.
    pub loader: Option<(u32, u32, u32)>,
    pub devices: Vec<Device>,
}

fn split(v: u32) -> (u32, u32, u32) {
    (
        vk::api_version_major(v),
        vk::api_version_minor(v),
        vk::api_version_patch(v),
    )
}

/// Ask the host, now. Costs one throwaway `VkInstance` (a few ms), so
/// anything that draws should hold the answer instead — [`cached`], or
/// the copy a window's model took when it opened.
pub fn probe() -> Probe {
    let none = |gpu| Probe {
        gpu,
        loader: None,
        devices: Vec::new(),
    };

    // SAFETY: `Entry::load` dlopens the loader; unsafe because a hostile
    // `libvulkan` on the search path could do anything. Same call the
    // player and every other Vulkan app make.
    let entry = match unsafe { ash::Entry::load() } {
        Ok(e) => e,
        Err(_) => return none(HostGpu::NoLoader),
    };

    // 1.0 loaders have no `vkEnumerateInstanceVersion` at all, which is
    // what `Ok(None)` means here.
    let loader = match unsafe { entry.try_enumerate_instance_version() } {
        Ok(Some(v)) => split(v),
        Ok(None) => (1, 0, 0),
        Err(_) => return none(HostGpu::NoLoader),
    };

    // Ask for no more than the loader has: DXVK asks for 1.3 flatly and
    // takes the `ERROR_INCOMPATIBLE_DRIVER`, but we want the device list
    // even from a host we are about to turn down.
    let want = if (loader.0, loader.1) >= (1, 3) {
        vk::API_VERSION_1_3
    } else {
        vk::API_VERSION_1_0
    };

    let portability = unsafe { entry.enumerate_instance_extension_properties(None) }
        .unwrap_or_default()
        .iter()
        .any(|e| e.extension_name_as_c_str() == Ok(vk::KHR_PORTABILITY_ENUMERATION_NAME));

    let app = vk::ApplicationInfo::default()
        .application_name(c"2ksbox host probe")
        .api_version(want);
    let exts = [vk::KHR_PORTABILITY_ENUMERATION_NAME.as_ptr()];
    let mut info = vk::InstanceCreateInfo::default().application_info(&app);
    if portability {
        info = info
            .enabled_extension_names(&exts)
            .flags(vk::InstanceCreateFlags::ENUMERATE_PORTABILITY_KHR);
    }

    // SAFETY: `info` and everything it points at outlive the call.
    let instance = match unsafe { entry.create_instance(&info, None) } {
        Ok(i) => i,
        Err(_) => {
            return Probe {
                gpu: if (loader.0, loader.1) >= (1, 3) {
                    HostGpu::NoDevice
                } else {
                    HostGpu::LoaderTooOld
                },
                loader: Some(loader),
                devices: Vec::new(),
            }
        }
    };

    // SAFETY: `instance` is live until `destroy_instance` below, and
    // nothing here keeps a handle past it.
    let devices: Vec<Device> = unsafe {
        instance
            .enumerate_physical_devices()
            .unwrap_or_default()
            .into_iter()
            .map(|pd| {
                let p = instance.get_physical_device_properties(pd);
                Device {
                    name: p
                        .device_name_as_c_str()
                        .map(|s| s.to_string_lossy().into_owned())
                        .unwrap_or_else(|_| "(unnamed)".into()),
                    kind: p.device_type,
                    api: split(p.api_version),
                }
            })
            .collect()
    };
    unsafe { instance.destroy_instance(None) };

    let gpu = if (loader.0, loader.1) < (1, 3) {
        // Nothing below can rescue this: DXVK's instance would not exist.
        HostGpu::LoaderTooOld
    } else if devices.is_empty() {
        HostGpu::NoDevice
    } else if devices.iter().any(|d| d.meets_bar() && !d.is_software()) {
        // A hardware device is what DXVK's own adapter order picks first,
        // so an extra software driver on the box changes nothing.
        HostGpu::Accelerated
    } else if devices.iter().any(|d| d.meets_bar()) {
        HostGpu::SoftwareOnly
    } else {
        HostGpu::DeviceTooOld
    };

    Probe {
        gpu,
        loader: Some(loader),
        devices,
    }
}

/// The same answer, probed once per process. Nothing about a host's GPU
/// changes while the launcher is open.
pub fn cached() -> &'static Probe {
    static ONCE: OnceLock<Probe> = OnceLock::new();
    ONCE.get_or_init(probe)
}

/// The report `--host-check` prints: the verdict, then every device the
/// loader offered and what it counted for.
pub fn report_text(p: &Probe) -> String {
    let mut s = String::new();
    s.push_str(&format!("Direct3D pass-through: {}\n", verdict_word(p.gpu)));
    s.push_str(&format!("{}\n", p.gpu.headline()));
    if let Some(a) = p.gpu.advice() {
        s.push_str(&format!("{}\n", a));
    }
    s.push('\n');
    match p.loader {
        Some((a, b, c)) => s.push_str(&format!("Vulkan loader: {a}.{b}.{c}\n")),
        None => s.push_str("Vulkan loader: not present\n"),
    }
    s.push_str("Required: a 1.3 device (DXVK 3.1's own bar, ADR-013)\n");
    if p.devices.is_empty() {
        s.push_str("Devices: none\n");
    } else {
        s.push_str("Devices:\n");
        for d in &p.devices {
            let (a, b, c) = d.api;
            let why = if !d.meets_bar() {
                "below 1.3"
            } else if d.is_software() {
                "software, usable but slow"
            } else {
                "meets the bar"
            };
            s.push_str(&format!(
                "  {} — {}, Vulkan {a}.{b}.{c} ({why})\n",
                d.name,
                d.kind_name()
            ));
        }
    }
    s
}

fn verdict_word(g: HostGpu) -> &'static str {
    match g {
        HostGpu::Accelerated => "available",
        HostGpu::SoftwareOnly => "available, in software (slow)",
        _ => "unavailable",
    }
}
