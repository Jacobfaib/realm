use std::cmp::max;
use std::collections::{BTreeMap, BTreeSet};
use std::sync::{Arc, Mutex};

use legion_prof_viewer::{
    data::{
        Color32, DataSource, DataSourceDescription, DataSourceInfo, EntryID, EntryInfo, Field,
        FieldID, FieldSchema, Item, ItemLink, ItemMeta, ItemUID, Rgba, SlotMetaTile,
        SlotMetaTileData, SlotTile, SlotTileData, SummaryTile, SummaryTileData, TileID, TileSet,
        UtilPoint,
    },
    timestamp as ts,
};

#[cfg(debug_assertions)]
use log::info;

use slice_group_by::GroupBy;

use crate::backend::common::{
    ChanEntryFieldsPretty, ChanEntryShort, DimOrderPretty, FSpaceShort, FieldsPretty, ISpacePretty,
    InstShort, MemGroup, ProcGroup, SizePretty, StatePostprocess,
};
use crate::conditional_assert;
use crate::state::{
    BacktraceID, ChanEntry, ChanID, Color, Config, Container, ContainerEntry, Copy, CopyInstInfo,
    DeviceKind, EventEntry, EventEntryKind, EventID, Fill, FillInstInfo, Inst, MemID, MemKind,
    NodeID, OpID, ProcEntryKind, ProcID, ProcKind, ProfUID, State, TimeRange, Timestamp,
};

impl Into<ts::Timestamp> for Timestamp {
    fn into(self) -> ts::Timestamp {
        ts::Timestamp(self.to_ns().try_into().unwrap())
    }
}

impl Into<Timestamp> for ts::Timestamp {
    fn into(self) -> Timestamp {
        Timestamp::from_ns(self.0.try_into().unwrap())
    }
}

impl Into<ts::Interval> for TimeRange {
    fn into(self) -> ts::Interval {
        ts::Interval::new(self.start.unwrap().into(), self.stop.unwrap().into())
    }
}

impl Into<ItemUID> for ProfUID {
    fn into(self) -> ItemUID {
        ItemUID(self.0)
    }
}

impl Into<Color32> for Color {
    fn into(self) -> Color32 {
        Color32::from_rgb(
            ((self.0 >> 16) & 0xFF) as u8,
            ((self.0 >> 8) & 0xFF) as u8,
            (self.0 & 0xFF) as u8,
        )
    }
}

#[derive(Debug, Clone)]
enum EntryKind {
    ProcKind(ProcGroup),
    Proc(ProcID, Option<DeviceKind>),
    MemKind(MemGroup),
    Mem(MemID),
    ChanKind(Option<NodeID>),
    Chan(ChanID),
    DepPartKind(Option<NodeID>),
    DepPart(ChanID),
}

#[derive(Debug, Clone)]
struct ItemInfo {
    point_interval: ts::Interval,
    expand: bool,
}

#[derive(Debug, Clone)]
pub struct Fields {
    chan_reqs: FieldID,
    expanded_for_visibility: FieldID,
    operation: FieldID,
    insts: FieldID,
    inst_fields: FieldID,
    inst_fspace: FieldID,
    inst_ispace: FieldID,
    inst_layout: FieldID,
    size: FieldID,
    interval: FieldID,
    num_items: FieldID,
    provenance: FieldID,
    status_ready: FieldID,
    status_running: FieldID,
    status_waiting: FieldID,
    deferred_time: FieldID,
    delayed_time: FieldID,
    creator: FieldID,
    caller: FieldID,
    callee: FieldID,
    mapper: FieldID,
    mapper_proc: FieldID,
    backtrace: FieldID,
    critical: FieldID,
    trigger_time: FieldID,
    previous_executing: FieldID,
    scheduling_overhead: FieldID,
    message_latency: FieldID,
}

#[derive(Debug)]
pub struct StateDataSource {
    state: State,
    field_schema: FieldSchema,
    fields: Fields,
    info: EntryInfo,
    entry_map: BTreeMap<EntryID, EntryKind>,
    proc_entries: BTreeMap<ProcID, EntryID>,
    proc_groups: BTreeMap<ProcGroup, Vec<ProcID>>,
    mem_entries: BTreeMap<MemID, EntryID>,
    mem_groups: BTreeMap<MemGroup, Vec<MemID>>,
    chan_entries: BTreeMap<ChanID, EntryID>,
    chan_groups: BTreeMap<Option<NodeID>, Vec<ChanID>>,
    deppart_groups: BTreeMap<Option<NodeID>, Vec<ChanID>>,
    step_utilization_cache: Mutex<BTreeMap<EntryID, Arc<Vec<(Timestamp, f64)>>>>,
}

impl StateDataSource {
    pub fn new(state: State) -> Self {
        let mut field_schema = FieldSchema::new();

        let fields = Fields {
            chan_reqs: field_schema.insert("Requirements".to_owned(), true),
            expanded_for_visibility: field_schema
                .insert("(Expanded for Visibility)".to_owned(), false),
            operation: field_schema.insert("Operation".to_owned(), true),
            insts: field_schema.insert("Instances".to_owned(), true),
            inst_fields: field_schema.insert("Fields".to_owned(), true),
            inst_fspace: field_schema.insert("Field Space".to_owned(), true),
            inst_ispace: field_schema.insert("Index Space".to_owned(), true),
            inst_layout: field_schema.insert("Layout".to_owned(), true),
            interval: field_schema.insert("Lifetime".to_owned(), false),
            num_items: field_schema.insert("Number of Items".to_owned(), false),
            provenance: field_schema.insert("Provenance".to_owned(), true),
            size: field_schema.insert("Size".to_owned(), true),
            status_ready: field_schema.insert("Ready".to_owned(), false),
            status_running: field_schema.insert("Running".to_owned(), false),
            status_waiting: field_schema.insert("Waiting".to_owned(), false),
            deferred_time: field_schema.insert("Deferred".to_owned(), false),
            delayed_time: field_schema.insert("Delayed".to_owned(), false),
            creator: field_schema.insert("Creator".to_owned(), false),
            caller: field_schema.insert("Caller".to_owned(), false),
            callee: field_schema.insert("Callee".to_owned(), false),
            mapper: field_schema.insert("Mapper".to_owned(), true),
            mapper_proc: field_schema.insert("Mapper Processor".to_owned(), true),
            backtrace: field_schema.insert("Backtrace".to_owned(), false),
            critical: field_schema.insert("Critical Path".to_owned(), true),
            trigger_time: field_schema.insert("Triggering Latency".to_owned(), false),
            previous_executing: field_schema.insert("Previous Executing".to_owned(), true),
            scheduling_overhead: field_schema.insert("Scheduling Overhead".to_owned(), false),
            message_latency: field_schema.insert("Message Latency".to_owned(), false),
        };

        let mut entry_map = BTreeMap::<EntryID, EntryKind>::new();
        let mut proc_entries = BTreeMap::new();
        let mut chan_entries = BTreeMap::new();
        let mut mem_entries = BTreeMap::new();

        let mut proc_groups = state.group_procs();
        let mem_groups = state.group_mems();
        let chan_groups = state.group_chans();
        let deppart_groups = state.group_depparts();

        let mut nodes: BTreeSet<_> = proc_groups.keys().map(|ProcGroup(n, _, _)| *n).collect();
        let proc_kinds: BTreeSet<_> = proc_groups
            .keys()
            .map(|ProcGroup(_, k, d)| (*k, *d))
            .collect();
        let mem_kinds: BTreeSet<_> = mem_groups.keys().map(|MemGroup(_, k)| *k).collect();

        if !state.has_multiple_nodes() {
            nodes.remove(&None);
        }

        let mut node_slots = Vec::new();
        let root_id = EntryID::root();
        for node in &nodes {
            let node_short_name;
            let node_long_name;
            match node {
                Some(node_id) => {
                    node_short_name = format!("n{}", node_id.0);
                    node_long_name = format!("Node {}", node_id.0);
                }
                None => {
                    node_short_name = "all".to_owned();
                    node_long_name = "All Nodes".to_owned();
                }
            }
            let node_index = node_slots.len() as u64;
            let node_id = root_id.child(node_index);

            let mut kind_slots = Vec::new();
            let mut kind_index = 0;
            let mut node_empty = node.is_some();
            // Processors
            for (kind, device) in &proc_kinds {
                let group = ProcGroup(*node, *kind, *device);

                // Not all kinds might exist on all nodes if the machine model
                // is not symmetric or if we didn't load some processors from
                // remote nodes when only loading logfiles from a subset of nodes
                if let Some(procs) = proc_groups.get(&group) {
                    if node.is_some() {
                        // Don't render kind if all processors of the kind are empty
                        let empty = procs.iter().all(|p| state.procs.get(p).unwrap().is_empty());
                        node_empty = node_empty && empty;
                        if empty {
                            continue;
                        }
                    }
                    let kind_name = format!("{:?}", kind);
                    let kind_first_letter = kind_name.chars().next().unwrap().to_lowercase();

                    let short_suffix = match device {
                        Some(DeviceKind::Device) => "d",
                        Some(DeviceKind::Host) => "h",
                        None => "",
                    };

                    let medium_suffix = match device {
                        Some(DeviceKind::Device) => " dev",
                        Some(DeviceKind::Host) => " host",
                        None => "",
                    };

                    let long_suffix = match device {
                        Some(DeviceKind::Device) => " Device",
                        Some(DeviceKind::Host) => " Host",
                        None => "",
                    };

                    let kind_id = node_id.child(kind_index);
                    kind_index += 1;

                    let color = match (kind, device) {
                        (ProcKind::GPU, Some(DeviceKind::Device)) => Color::OLIVEDRAB,
                        (ProcKind::GPU, Some(DeviceKind::Host)) => Color::ORANGERED,
                        (ProcKind::CPU, None) => Color::STEELBLUE,
                        (ProcKind::Utility, None) => Color::CRIMSON,
                        (ProcKind::IO, None) => Color::ORANGERED,
                        (ProcKind::ProcGroup, None) => Color::ORANGERED,
                        (ProcKind::ProcSet, None) => Color::ORANGERED,
                        (ProcKind::OpenMP, None) => Color::ORANGERED,
                        (ProcKind::Python, None) => Color::OLIVEDRAB,
                        _ => unreachable!(),
                    };
                    let color: Color32 = color.into();

                    let mut proc_slots = Vec::new();
                    if node.is_some() {
                        let mut proc_index = 0;
                        for proc in procs {
                            let proc_id = kind_id.child(proc_index as u64);
                            entry_map.insert(proc_id.clone(), EntryKind::Proc(*proc, *device));
                            proc_entries.insert(*proc, proc_id);

                            let short_name = format!(
                                "{}{}{}",
                                kind_first_letter,
                                proc.proc_in_node(),
                                short_suffix
                            );
                            let long_name = format!(
                                "{} {} {}{}",
                                node_long_name,
                                kind_name,
                                proc.proc_in_node(),
                                long_suffix
                            );

                            let max_rows =
                                state.procs.get(proc).unwrap().max_levels(*device) as u64 + 1;
                            proc_slots.push(EntryInfo::Slot {
                                short_name,
                                long_name,
                                max_rows,
                            });
                            proc_index += 1;
                        }
                    }

                    let summary_id = kind_id.summary();
                    entry_map.insert(summary_id, EntryKind::ProcKind(group));

                    kind_slots.push(EntryInfo::Panel {
                        short_name: format!("{}{}", kind_name.to_lowercase(), medium_suffix),
                        long_name: format!("{} {}{}", node_long_name, kind_name, long_suffix),
                        summary: Some(Box::new(EntryInfo::Summary { color })),
                        slots: proc_slots,
                    });
                }
            }

            // Don't render node if all processors of the node are empty
            if node_empty {
                // Remove this node's processors from the all nodes list to
                // avoid influencing global utilization
                for (kind, device) in &proc_kinds {
                    let group = ProcGroup(None, *kind, *device);
                    proc_groups
                        .get_mut(&group)
                        .unwrap()
                        .retain(|p| p.node_id() != node.unwrap());
                }
                continue;
            }

            // Memories
            for kind in &mem_kinds {
                let group = MemGroup(*node, *kind);

                let Some(mems) = mem_groups.get(&group) else {
                    continue;
                };

                let kind_name = format!("{:?}", kind);
                let kind_first_letter = kind_name.chars().next().unwrap().to_lowercase();

                let kind_id = node_id.child(kind_index);
                kind_index += 1;

                let color = match kind {
                    MemKind::NoMemKind => unreachable!(),
                    MemKind::Global => Color::CRIMSON,
                    MemKind::System => Color::OLIVEDRAB,
                    MemKind::Registered => Color::DARKMAGENTA,
                    MemKind::Socket => Color::ORANGERED,
                    MemKind::ZeroCopy => Color::CRIMSON,
                    MemKind::Framebuffer => Color::BLUE,
                    MemKind::Disk => Color::DARKGOLDENROD,
                    MemKind::HDF5 => Color::OLIVEDRAB,
                    MemKind::File => Color::ORANGERED,
                    MemKind::L3Cache => Color::CRIMSON,
                    MemKind::L2Cache => Color::DARKMAGENTA,
                    MemKind::L1Cache => Color::OLIVEDRAB,
                    MemKind::GPUManaged => Color::DARKMAGENTA,
                    MemKind::GPUDynamic => Color::ORANGERED,
                };
                let color: Color32 = color.into();

                let mut mem_slots = Vec::new();
                if node.is_some() {
                    for (mem_index, mem) in mems.iter().enumerate() {
                        let mem_id = kind_id.child(mem_index as u64);
                        entry_map.insert(mem_id.clone(), EntryKind::Mem(*mem));
                        mem_entries.insert(*mem, mem_id);

                        let rows = state.mems.get(mem).unwrap().max_levels(None) as u64 + 1;
                        mem_slots.push(EntryInfo::Slot {
                            short_name: format!("{}{}", kind_first_letter, mem.mem_in_node()),
                            long_name: format!(
                                "{} {} {}",
                                node_long_name,
                                kind_name,
                                mem.mem_in_node()
                            ),
                            max_rows: rows,
                        });
                    }
                }

                let summary_id = kind_id.summary();
                entry_map.insert(summary_id, EntryKind::MemKind(group));

                kind_slots.push(EntryInfo::Panel {
                    short_name: kind_name.to_lowercase(),
                    long_name: format!("{} {}", node_long_name, kind_name),
                    summary: Some(Box::new(EntryInfo::Summary { color })),
                    slots: mem_slots,
                });
            }

            // Channels (except for Dependent Partitioning)
            loop {
                let Some(chans) = chan_groups.get(node) else {
                    break;
                };

                let kind_id = node_id.child(kind_index);
                kind_index += 1;

                let color: Color32 = Color::ORANGERED.into();

                let mut chan_slots = Vec::new();
                if node.is_some() {
                    for (chan_index, chan) in chans.iter().enumerate() {
                        let chan_id = kind_id.child(chan_index as u64);
                        entry_map.insert(chan_id.clone(), EntryKind::Chan(*chan));
                        chan_entries.insert(*chan, chan_id);

                        let (src_name, src_short) = match chan {
                            ChanID::Copy { src, .. } | ChanID::Scatter { src } => {
                                let kind = state.mems.get(&src).unwrap().kind;
                                let kind_first_letter =
                                    format!("{:?}", kind).chars().next().unwrap().to_lowercase();
                                let src_node = src.node_id().0;
                                (
                                    Some(format!(
                                        "Node {} {:?} {}",
                                        src_node,
                                        kind,
                                        src.mem_in_node()
                                    )),
                                    Some(format!(
                                        "n{}{}{}",
                                        src_node,
                                        kind_first_letter,
                                        src.mem_in_node()
                                    )),
                                )
                            }
                            _ => (None, None),
                        };

                        let (dst_name, dst_short) = match chan {
                            ChanID::Copy { dst, .. }
                            | ChanID::Fill { dst }
                            | ChanID::Gather { dst } => {
                                let kind = state.mems.get(&dst).unwrap().kind;
                                let kind_first_letter =
                                    format!("{:?}", kind).chars().next().unwrap().to_lowercase();
                                let dst_node = dst.node_id().0;
                                (
                                    Some(format!(
                                        "Node {} {:?} {}",
                                        dst_node,
                                        kind,
                                        dst.mem_in_node()
                                    )),
                                    Some(format!(
                                        "n{}{}{}",
                                        dst_node,
                                        kind_first_letter,
                                        dst.mem_in_node()
                                    )),
                                )
                            }
                            _ => (None, None),
                        };

                        let short_name = match chan {
                            ChanID::Copy { .. } => {
                                format!("{}-{}", src_short.unwrap(), dst_short.unwrap())
                            }
                            ChanID::Fill { .. } => format!("f {}", dst_short.unwrap()),
                            ChanID::Gather { .. } => format!("g {}", dst_short.unwrap()),
                            ChanID::Scatter { .. } => format!("s {}", src_short.unwrap()),
                            ChanID::DepPart { .. } => unreachable!(),
                        };

                        let long_name = match chan {
                            ChanID::Copy { .. } => {
                                format!("{} to {}", src_name.unwrap(), dst_name.unwrap())
                            }
                            ChanID::Fill { .. } => format!("Fill {}", dst_name.unwrap()),
                            ChanID::Gather { .. } => format!("Gather to {}", dst_name.unwrap()),
                            ChanID::Scatter { .. } => {
                                format!("Scatter from {}", src_name.unwrap())
                            }
                            ChanID::DepPart { .. } => unreachable!(),
                        };

                        let rows = state.chans.get(chan).unwrap().max_levels(None) as u64 + 1;
                        chan_slots.push(EntryInfo::Slot {
                            short_name,
                            long_name,
                            max_rows: rows,
                        });
                    }
                }

                let summary_id = kind_id.summary();
                entry_map.insert(summary_id, EntryKind::ChanKind(*node));

                kind_slots.push(EntryInfo::Panel {
                    short_name: "chan".to_owned(),
                    long_name: format!("{} Channel", node_long_name),
                    summary: Some(Box::new(EntryInfo::Summary { color })),
                    slots: chan_slots,
                });

                break;
            }

            // Dependent Partitioning Channels
            loop {
                let Some(chans) = deppart_groups.get(node) else {
                    break;
                };

                let kind_id = node_id.child(kind_index);

                let color: Color32 = Color::ORANGERED.into();

                let mut deppart_slots = Vec::new();
                if node.is_some() {
                    for (chan_index, chan) in chans.iter().enumerate() {
                        let chan_id = kind_id.child(chan_index as u64);
                        entry_map.insert(chan_id.clone(), EntryKind::DepPart(*chan));
                        chan_entries.insert(*chan, chan_id);

                        let short_name = match chan {
                            ChanID::DepPart { node_id } => format!("dp{}", node_id.0),
                            _ => unreachable!(),
                        };

                        let long_name = match chan {
                            ChanID::DepPart { node_id } => {
                                format!("Dependent Partitioning {}", node_id.0)
                            }
                            _ => unreachable!(),
                        };

                        let rows = state.chans.get(chan).unwrap().max_levels(None) as u64 + 1;
                        deppart_slots.push(EntryInfo::Slot {
                            short_name,
                            long_name,
                            max_rows: rows,
                        });
                    }
                }

                let summary_id = kind_id.summary();
                entry_map.insert(summary_id, EntryKind::DepPartKind(*node));

                kind_slots.push(EntryInfo::Panel {
                    short_name: "dp".to_owned(),
                    long_name: format!("{} Dependent Partitioning", node_long_name),
                    summary: Some(Box::new(EntryInfo::Summary { color })),
                    slots: deppart_slots,
                });

                break;
            }
            node_slots.push(EntryInfo::Panel {
                short_name: node_short_name,
                long_name: node_long_name,
                summary: None,
                slots: kind_slots,
            });
        }

        let info = EntryInfo::Panel {
            short_name: "root".to_owned(),
            long_name: "root".to_owned(),
            summary: None,
            slots: node_slots,
        };

        Self {
            state,
            field_schema,
            fields,
            info,
            entry_map,
            proc_entries,
            proc_groups,
            mem_entries,
            mem_groups,
            chan_entries,
            chan_groups,
            deppart_groups,
            step_utilization_cache: Mutex::new(BTreeMap::new()),
        }
    }
}

impl StateDataSource {
    /// A step utilization is a series of step functions. At time T, the
    /// utilization takes value U. That value continues until the next
    /// step. This is a good match for Legion's discrete execution model (a
    /// task is either running, or it is not), but doesn't play so well with
    /// interpolation and level of detail. We compute this first because it's
    /// how the profiler internally represents utilization, but we convert it
    /// to a more useful format below.
    fn generate_step_utilization(&self, entry_id: &EntryID) -> Arc<Vec<(Timestamp, f64)>> {
        // This is an INTENTIONAL race; if two requests for the same entry
        // arrive simultaneously, we'll miss in the cache on both and compute
        // the utilization twice. The result should be the same, so this is
        // mostly wasted computation (in exchange for enabling parallelism).

        let cache = &self.step_utilization_cache;
        if let Some(util) = cache.lock().unwrap().get(entry_id) {
            return util.clone();
        }

        let group_kind = self.entry_map.get(entry_id).unwrap();
        let step_utilization = match group_kind {
            EntryKind::ProcKind(group) => {
                let ProcGroup(_, _, device) = *group;
                let procs = self.proc_groups.get(group).unwrap();
                let points = self.state.proc_group_timepoints(device, procs);
                let count = procs.len() as u64;
                let owners: BTreeSet<_> = procs
                    .iter()
                    .zip(points.iter())
                    .filter(|(_, tp)| !tp.is_empty())
                    .map(|(proc_id, _)| *proc_id)
                    .collect();

                if owners.is_empty() {
                    Vec::new()
                } else {
                    let mut utilizations = Vec::new();
                    for tp in points {
                        if !tp.is_empty() {
                            self.state
                                .convert_points_to_utilization(tp, &mut utilizations);
                        }
                    }
                    utilizations.sort_by_key(|point| point.time_key());
                    self.state
                        .calculate_proc_utilization_data(utilizations, owners, count)
                }
            }
            EntryKind::MemKind(group) => {
                let mems = self.mem_groups.get(group).unwrap();
                let points = self.state.mem_group_timepoints(mems);
                let owners: BTreeSet<_> = mems
                    .iter()
                    .zip(points.iter())
                    .filter(|(_, tp)| !tp.is_empty())
                    .map(|(mem_id, _)| *mem_id)
                    .collect();

                if owners.is_empty() {
                    Vec::new()
                } else {
                    let mut utilizations: Vec<_> = points
                        .iter()
                        .filter(|tp| !tp.is_empty())
                        .flat_map(|tp| *tp)
                        .collect();
                    utilizations.sort_by_key(|point| point.time_key());
                    self.state
                        .calculate_mem_utilization_data(utilizations, owners)
                }
            }
            EntryKind::ChanKind(node) | EntryKind::DepPartKind(node) => {
                let chans = match group_kind {
                    EntryKind::ChanKind(..) => self.chan_groups.get(node).unwrap(),
                    EntryKind::DepPartKind(..) => self.deppart_groups.get(node).unwrap(),
                    _ => unreachable!(),
                };
                let points = self.state.chan_group_timepoints(chans);
                let owners: BTreeSet<_> = chans
                    .iter()
                    .zip(points.iter())
                    .filter(|(_, tp)| !tp.is_empty())
                    .map(|(chan_id, _)| *chan_id)
                    .collect();

                if owners.is_empty() {
                    Vec::new()
                } else {
                    let mut utilizations = Vec::new();
                    for tp in points {
                        if !tp.is_empty() {
                            self.state
                                .convert_points_to_utilization(tp, &mut utilizations);
                        }
                    }
                    utilizations.sort_by_key(|point| point.time_key());
                    self.state
                        .calculate_chan_utilization_data(utilizations, owners)
                }
            }
            _ => unreachable!(),
        };
        let result = Arc::new(step_utilization);
        cache
            .lock()
            .unwrap()
            .insert(entry_id.clone(), result.clone());
        result
    }

    /// Converts the step utilization into a sample utilization, where each
    /// utilization point (sample) represents the average utilization over a
    /// certain time interval. The sample is located in the middle of the
    /// interval.
    fn compute_sample_utilization(
        step_utilization: &Vec<(Timestamp, f64)>,
        interval: ts::Interval,
        samples: u64,
    ) -> Vec<UtilPoint> {
        let start_time = interval.start.0 as u64;
        let duration = interval.duration_ns() as u64;

        let first_index = step_utilization
            .partition_point(|&(t, _)| {
                let t: ts::Timestamp = t.into();
                t < interval.start
            })
            .saturating_sub(1);

        let mut last_index = step_utilization[first_index..].partition_point(|&(t, _)| {
            let t: ts::Timestamp = t.into();
            t < interval.stop
        }) + first_index;
        if last_index + 1 < step_utilization.len() {
            last_index = last_index + 1;
        }

        let mut utilization = Vec::new();
        let mut last_t = 0u64;
        let mut last_u = 0.0;
        let mut step_it = step_utilization[first_index..last_index].iter().peekable();
        for sample in 0..samples {
            let sample_start = duration * sample / samples + start_time;
            let sample_stop = duration * (sample + 1) / samples + start_time;
            if sample_stop - sample_start == 0 {
                continue;
            }

            let mut sample_util = 0.0;
            while let Some((t, u)) = step_it.next_if(|(t, _)| t.to_ns() < sample_stop) {
                if t.to_ns() < sample_start {
                    (last_t, last_u) = (t.to_ns(), *u);
                    continue;
                }

                // This is a step utilization. So utilization u begins on time
                // t. That means the previous utilization stop at time t-1.
                let last_duration = (t.to_ns() - 1).saturating_sub(last_t.max(sample_start));
                sample_util += last_duration as f64 * last_u;

                (last_t, last_u) = (t.to_ns(), *u);
            }
            if last_t < sample_stop {
                let last_duration = sample_stop - last_t.max(sample_start);
                sample_util += last_duration as f64 * last_u;
            }

            sample_util = sample_util / (sample_stop - sample_start) as f64;
            assert!(sample_util <= 1.0);
            utilization.push(UtilPoint {
                time: Timestamp::from_ns((sample_start + sample_stop) / 2).into(),
                util: sample_util as f32,
            });
        }
        utilization
    }

    /// Items smaller than this should be expanded (and merged, if suitable
    /// nearby items are found)
    const MAX_RATIO: f64 = 2000.0;

    /// Items larger than this should NOT be merged, even if nearby an expanded
    /// item
    const MIN_RATIO: f64 = 1000.0;

    /// Expand small items to improve visibility
    fn expand_item(
        interval: &mut ts::Interval,
        tile_id: TileID,
        last: Option<&Item>,
        merged: u64,
    ) -> bool {
        let view_ratio = tile_id.0.duration_ns() as f64 / interval.duration_ns() as f64;

        let expand = view_ratio > Self::MAX_RATIO;
        if expand {
            let min_duration = tile_id.0.duration_ns() as f64 / Self::MAX_RATIO;
            let center = (interval.start.0 + interval.stop.0) as f64 / 2.0;
            let start = ts::Timestamp((center - min_duration / 2.0) as i64);
            let stop = ts::Timestamp(start.0 + min_duration as i64);
            *interval = ts::Interval::new(start, stop);

            // If the previous task is large (and overlaps), shrink to avoid overlapping it
            if let Some(last) = last {
                let last_ratio =
                    tile_id.0.duration_ns() as f64 / last.interval.duration_ns() as f64;
                if interval.overlaps(last.interval) && last_ratio < Self::MIN_RATIO {
                    if merged > 0 {
                        // It's already a merged task, ok to keep merging
                    } else {
                        interval.start = last.interval.stop;
                    }
                }
            }
        }
        expand
    }

    /// Merge small tasks to reduce load on renderer
    fn merge_items(
        interval: ts::Interval,
        tile_id: TileID,
        last: &mut Item,
        last_meta: Option<&mut ItemMeta>,
        num_items_field: FieldID,
        merged: &mut u64,
    ) -> bool {
        // Check for overlap with previous task. If so, either one or the
        // other task was expanded (since tasks don't normally overlap)
        // and this is a good opportunity to combine them.
        if last.interval.overlaps(interval) {
            // If the current task is large, don't merge. Instead,
            // just modify the previous task so it doesn't overlap
            let view_ratio = tile_id.0.duration_ns() as f64 / interval.duration_ns() as f64;
            if view_ratio < Self::MIN_RATIO {
                last.interval.stop = interval.start;
            } else {
                last.interval.stop = interval.stop;
                last.color = Color::GRAY.into();
                if let Some(last_meta) = last_meta {
                    if let Some((_, Field::U64(value), _)) = last_meta.fields.get_mut(0) {
                        *value += 1;
                    } else {
                        last_meta.title = "Merged Tasks".to_owned();
                        last_meta.fields = vec![(num_items_field, Field::U64(2), None)];
                    }
                }
                *merged += 1;
                return true;
            }
        }
        *merged = 0;
        false
    }

    fn build_items<C>(
        &self,
        cont: &C,
        device: Option<DeviceKind>,
        tile_id: TileID,
        full: bool,
        mut item_metas: Option<&mut Vec<Vec<ItemMeta>>>,
        get_meta: impl Fn(&C::Entry, ItemInfo) -> ItemMeta,
    ) -> Vec<Vec<Item>>
    where
        C: Container,
    {
        let mut items: Vec<Vec<Item>> = Vec::new();
        let mut merged = Vec::new();
        let levels = cont.max_levels(device) as usize + 1;
        items.resize_with(levels, Vec::new);
        if let Some(ref mut item_metas) = item_metas {
            item_metas.resize_with(levels, Vec::new);
        }
        merged.resize(levels, 0u64);
        let points_stacked = cont.time_points_stacked(device);

        for (level, points) in points_stacked.iter().enumerate() {
            let items = &mut items[level];
            let mut item_metas = item_metas.as_mut().map(|m| &mut m[level]);
            let merged = &mut merged[level];

            let first_index = points.partition_point(|p| {
                let stop: ts::Timestamp = cont.entry(p.entry).time_range().stop.unwrap().into();
                ts::Timestamp(stop.0.saturating_sub(1)) < tile_id.0.start
            });
            let last_index = points[first_index..].partition_point(|p| {
                let start: ts::Timestamp = cont.entry(p.entry).time_range().start.unwrap().into();
                start < tile_id.0.stop
            }) + first_index;

            #[cfg(debug_assertions)]
            {
                info!("Debug assertions enabled: checking point overlap. This can be expensive.");
                for point in &points[..first_index] {
                    let time_range = cont.entry(point.entry).time_range();
                    let point_interval: ts::Interval = time_range.into();
                    assert!(!point_interval.overlaps(tile_id.0));
                }
                for point in &points[last_index..] {
                    let time_range = cont.entry(point.entry).time_range();
                    let point_interval: ts::Interval = time_range.into();
                    assert!(!point_interval.overlaps(tile_id.0));
                }
            }

            for point in &points[first_index..last_index] {
                assert!(point.first);

                let entry = cont.entry(point.entry);
                let (base, time_range, waiters) =
                    (&entry.base(), entry.time_range(), &entry.waiters());

                let point_interval: ts::Interval = time_range.into();
                assert!(point_interval.overlaps(tile_id.0));
                let mut view_interval = point_interval.intersection(tile_id.0);

                assert_eq!(level, base.level.unwrap() as usize);

                let expand =
                    !full && Self::expand_item(&mut view_interval, tile_id, items.last(), *merged);

                if let Some(last) = items.last_mut() {
                    let last_meta = if let Some(ref mut item_metas) = item_metas {
                        item_metas.last_mut()
                    } else {
                        None
                    };
                    if Self::merge_items(
                        view_interval,
                        tile_id,
                        last,
                        last_meta,
                        self.fields.num_items,
                        merged,
                    ) {
                        continue;
                    }
                }

                let color = entry.color(&self.state);
                let color: Color32 = color.into();
                let color: Rgba = color.into();

                let item_meta = item_metas.as_ref().map(|_| {
                    get_meta(
                        entry,
                        ItemInfo {
                            point_interval,
                            expand,
                        },
                    )
                });

                let mut add_item =
                    |interval: ts::Interval,
                     opacity: f32,
                     status: Option<FieldID>,
                     wait_callee: Option<ProfUID>,
                     wait_backtrace: Option<BacktraceID>,
                     wait_event: Option<EventID>,
                     find_previous_executing: bool| {
                        if !interval.overlaps(tile_id.0) {
                            return;
                        }
                        let view_interval = interval.intersection(tile_id.0);
                        let color =
                            (Rgba::WHITE.multiply(1.0 - opacity) + color.multiply(opacity)).into();
                        let item = Item {
                            item_uid: base.prof_uid.into(),
                            interval: view_interval,
                            color,
                        };
                        items.push(item);
                        if let Some(ref mut item_metas) = item_metas {
                            let mut item_meta = item_meta.clone().unwrap();
                            if let Some(status) = status {
                                item_meta
                                    .fields
                                    .insert(1, (status, Field::Interval(interval), None));
                            }
                            if let Some(callee) = wait_callee {
                                item_meta.fields.push((
                                    self.fields.callee,
                                    self.generate_proc_link(callee),
                                    None,
                                ));
                            }
                            if let Some(backtrace) = wait_backtrace {
                                item_meta.fields.push((
                                    self.fields.backtrace,
                                    Field::String(
                                        self.state.backtraces.get(&backtrace).unwrap().to_string(),
                                    ),
                                    None,
                                ));
                            }
                            if let Some(event) = wait_event {
                                if let Some(event_entry) = self.state.find_critical_entry(event) {
                                    item_meta.fields.push((
                                        self.fields.critical,
                                        self.generate_critical_link(event, event_entry),
                                        self.select_critical_color(event_entry),
                                    ));
                                    // Record the time it took for Realm to propagate the event trigger
                                    if event_entry.kind != EventEntryKind::UnknownEvent {
                                        let trigger_time = event_entry.trigger_time.unwrap();
                                        item_meta.fields.push((
                                            self.fields.trigger_time,
                                            Field::Interval(ts::Interval::new(
                                                trigger_time.into(),
                                                interval.stop,
                                            )),
                                            self.select_interval_color(
                                                trigger_time,
                                                interval.stop.into(),
                                            ),
                                        ));
                                    }
                                } else {
                                    if event.is_barrier() {
                                        item_meta.fields.push((
                                                self.fields.critical,
                                                Field::String(format!("Waiting on unknown critical path barrier {:#x} created on node {}. Please load the logfile from at least one node that arrives on this barrier to start determining a critical path. You'll need to load the logs from all nodes that arrive on this barrier to determine a precise critical path. If you see this message and did not run with the -lg:prof_all_critical_arrivals flag then please report this case as it is likely a bug.", event.0, event.node_id().0)), 
                                                Some(Color32::BLUE)));
                                    } else {
                                        item_meta.fields.push((
                                                self.fields.critical,
                                                Field::String(format!("Waiting on unknown critical path event {:#x} from node {}. Please load the logfile from that node to see it.", event.0, event.node_id().0)),
                                                Some(Color32::BLUE)));
                                    }
                                }
                            }
                            if find_previous_executing {
                                // For ready intervals, find the last running range before this
                                // task can resume and record that as the previous executing field
                                if let Some((previous, prev_start, prev_stop)) = cont
                                    .find_previous_executing_entry(
                                        interval.start.into(),
                                        interval.stop.into(),
                                        device,
                                    )
                                {
                                    item_meta.fields.push((
                                        self.fields.previous_executing,
                                        self.generate_previous_executing_link(
                                            previous, prev_start, prev_stop,
                                        ),
                                        None,
                                    ));
                                    item_meta.fields.push((
                                        self.fields.scheduling_overhead,
                                        Field::Interval(ts::Interval::new(
                                            prev_stop.into(),
                                            interval.start,
                                        )),
                                        self.select_interval_color(prev_stop, interval.stop.into()),
                                    ));
                                }
                            }
                            item_metas.push(item_meta);
                        }
                    };
                if let Some(waiters) = waiters {
                    let mut start = time_range.start.unwrap();
                    for wait in &waiters.wait_intervals {
                        let running_interval = ts::Interval::new(start.into(), wait.start.into());
                        let waiting_interval =
                            ts::Interval::new(wait.start.into(), wait.ready.into());
                        let ready_interval = ts::Interval::new(wait.ready.into(), wait.end.into());
                        add_item(
                            running_interval,
                            1.0,
                            Some(self.fields.status_running),
                            None,
                            None,
                            None,
                            false,
                        );
                        add_item(
                            waiting_interval,
                            0.15,
                            Some(self.fields.status_waiting),
                            wait.callee,
                            wait.backtrace,
                            wait.event,
                            false,
                        );
                        add_item(
                            ready_interval,
                            0.45,
                            Some(self.fields.status_ready),
                            None,
                            None,
                            None,
                            true,
                        );
                        start = max(start, wait.end);
                    }
                    let stop = time_range.stop.unwrap();
                    if start < stop {
                        let running_interval = ts::Interval::new(start.into(), stop.into());
                        add_item(
                            running_interval,
                            1.0,
                            Some(self.fields.status_running),
                            None,
                            None,
                            None,
                            false,
                        );
                    }
                } else {
                    add_item(view_interval, 1.0, None, None, None, None, false);
                }
            }
        }
        items
    }

    fn generate_proc_slot_tile(
        &self,
        entry_id: &EntryID,
        proc_id: ProcID,
        device: Option<DeviceKind>,
        tile_id: TileID,
        full: bool,
    ) -> SlotTile {
        let proc = self.state.procs.get(&proc_id).unwrap();
        let items = self.build_items(proc, device, tile_id, full, None, |_, _| unreachable!());
        SlotTile {
            entry_id: entry_id.clone(),
            tile_id,
            data: SlotTileData { items },
        }
    }

    fn generate_op_link(&self, op_id: OpID) -> Field {
        if let Some(proc_id) = self.state.tasks.get(&op_id) {
            if let Some(proc) = self.state.procs.get(proc_id) {
                let op = proc.find_task(op_id).unwrap();
                let op_name = op.name(&self.state);
                return Field::ItemLink(ItemLink {
                    item_uid: op.base().prof_uid.into(),
                    title: op_name,
                    interval: op.time_range().into(),
                    entry_id: self.proc_entries.get(proc_id).unwrap().clone(),
                });
            }
        }
        if let Some(task) = self.state.multi_tasks.get(&op_id) {
            if let Some(kind) = self.state.task_kinds.get(&task.task_id) {
                if let Some(name) = &kind.name {
                    return Field::String(format!("Task {}<{}>", name, op_id.0));
                }
            }
        }
        if let Some(op) = self.state.find_op(op_id) {
            if let Some(kind) = op.kind {
                return Field::String(format!(
                    "{} Operation<{}>",
                    self.state.op_kinds[&kind].name, op_id.0
                ));
            }
        }
        Field::U64(op_id.0.get())
    }

    fn generate_inst_link(&self, inst_uid: ProfUID, prefix: &str) -> Option<Field> {
        let mem_id = self.state.insts.get(&inst_uid)?;
        let mem = self.state.mems.get(mem_id)?;
        let inst = mem.insts.get(&inst_uid)?;

        Some(Field::ItemLink(ItemLink {
            item_uid: inst.base().prof_uid.into(),
            title: format!("{}0x{:x}", prefix, inst.inst_id.unwrap().0),
            interval: inst.time_range().into(),
            entry_id: self.mem_entries.get(mem_id).unwrap().clone(),
        }))
    }

    fn generate_proc_link(&self, prof_uid: ProfUID) -> Field {
        // We should always be able to find the processor in this case
        let proc_id = self.state.prof_uid_proc.get(&prof_uid).unwrap();
        let proc = self.state.procs.get(&proc_id).unwrap();
        let entry = proc.find_entry(prof_uid).unwrap();
        let op_name = entry.name(&self.state);
        Field::ItemLink(ItemLink {
            item_uid: entry.base().prof_uid.into(),
            title: op_name,
            interval: entry.time_range().into(),
            entry_id: self.proc_entries.get(proc_id).unwrap().clone(),
        })
    }

    // Use this function to generate a link to the creator of an operation
    // Note that you give the timestamp so we can find the precise entry inside
    // of the creator that actually created the object
    fn generate_creator_link(&self, prof_uid: ProfUID, creation_time: Timestamp) -> Field {
        // Not all ProfUIDs will have a processor since some of them
        // might be referering to fevents that we never found
        if let Some(proc_id) = self.state.prof_uid_proc.get(&prof_uid) {
            let proc = self.state.procs.get(&proc_id).unwrap();
            // The prof_uid here is the fevent creator, find the entry that was actually
            // executing during this task at the point of creation
            let entry = proc.find_executing_entry(prof_uid, creation_time).unwrap();
            let op_name = entry.name(&self.state);
            Field::ItemLink(ItemLink {
                item_uid: entry.base().prof_uid.into(),
                title: op_name,
                interval: entry.time_range().into(),
                entry_id: self.proc_entries.get(proc_id).unwrap().clone(),
            })
        } else if let Some(chan_id) = self.state.prof_uid_chan.get(&prof_uid) {
            let chan = self.state.chans.get(&chan_id).unwrap();
            let entry = chan.find_entry(prof_uid).unwrap();
            let op_name = entry.name(&self.state);
            Field::ItemLink(ItemLink {
                item_uid: entry.base().prof_uid.into(),
                title: op_name,
                interval: entry.time_range().into(),
                entry_id: self.chan_entries.get(chan_id).unwrap().clone(),
            })
        } else if let Some(mem_id) = self.state.insts.get(&prof_uid) {
            let mem = self.state.mems.get(&mem_id).unwrap();
            let inst = mem.entry(prof_uid);
            let inst_name = inst.name(&self.state);
            Field::ItemLink(ItemLink {
                item_uid: inst.base().prof_uid.into(),
                title: inst_name,
                interval: inst.time_range().into(),
                entry_id: self.mem_entries.get(mem_id).unwrap().clone(),
            })
        } else {
            // Convert the ProfUID back into an fevent so we can figure
            // out which node it is on and tell the user that they need
            // to load the logfile from that node if they want to see it
            let node = self.state.find_fevent(prof_uid).node_id();
            Field::String(format!(
                "Unknown creator on node {}. Please load the logfile from that node to see it.",
                node.0
            ))
        }
    }

    // Use this function when the critical path is the previous creator of an operation
    fn generate_critical_creator_link(&self, prof_uid: ProfUID, creation_time: Timestamp) -> Field {
        // Not all ProfUIDs will have a processor since some of them
        // might be referering to fevents that we never found
        let creation_ts: ts::Timestamp = creation_time.into();
        if let Some(proc_id) = self.state.prof_uid_proc.get(&prof_uid) {
            let proc = self.state.procs.get(&proc_id).unwrap();
            // The prof_uid here is the fevent creator, find the entry that was actually
            // executing during this task at the point of creation
            let entry = proc.find_executing_entry(prof_uid, creation_time).unwrap();
            let op_name = entry.name(&self.state);
            let proc_name = proc.name(&self.state);
            Field::ItemLink(ItemLink {
                item_uid: entry.base().prof_uid.into(),
                title: format!(
                    "Created by {} at {} on {}",
                    &op_name, creation_ts, proc_name
                ),
                interval: entry.time_range().into(),
                entry_id: self.proc_entries.get(proc_id).unwrap().clone(),
            })
        } else if let Some(chan_id) = self.state.prof_uid_chan.get(&prof_uid) {
            let chan = self.state.chans.get(&chan_id).unwrap();
            let entry = chan.find_entry(prof_uid).unwrap();
            let op_name = entry.name(&self.state);
            let chan_name = chan.name(&self.state);
            Field::ItemLink(ItemLink {
                item_uid: entry.base().prof_uid.into(),
                title: format!(
                    "Created by {} at {} in {}",
                    &op_name, creation_ts, chan_name
                ),
                interval: entry.time_range().into(),
                entry_id: self.chan_entries.get(chan_id).unwrap().clone(),
            })
        } else if let Some(mem_id) = self.state.insts.get(&prof_uid) {
            let mem = self.state.mems.get(&mem_id).unwrap();
            let inst = mem.entry(prof_uid);
            let inst_name = inst.name(&self.state);
            let mem_name = mem.name(&self.state);
            Field::ItemLink(ItemLink {
                item_uid: inst.base().prof_uid.into(),
                title: format!(
                    "Created by {} at {} in {}",
                    &inst_name, creation_ts, mem_name
                ),
                interval: inst.time_range().into(),
                entry_id: self.mem_entries.get(mem_id).unwrap().clone(),
            })
        } else {
            // Convert the ProfUID back into an fevent so we can figure
            // out which node it is on and tell the user that they need
            // to load the logfile from that node if they want to see it
            let node = self.state.find_fevent(prof_uid).node_id();
            Field::String(format!(
                "Unknown creator on node {}. Please load the logfile from that node to see it.",
                node.0
            ))
        }
    }

    // Use this function when the critical path is the previous executing range
    // on the same processor
    fn generate_previous_executing_link(
        &self,
        previous: ProfUID,
        start: Timestamp,
        stop: Timestamp,
    ) -> Field {
        let proc_id = self.state.prof_uid_proc.get(&previous).unwrap();
        let proc = self.state.procs.get(&proc_id).unwrap();
        let entry = proc.find_entry(previous).unwrap();
        let op_name = entry.name(&self.state);
        Field::ItemLink(ItemLink {
            item_uid: entry.base().prof_uid.into(),
            title: op_name,
            interval: ts::Interval::new(start.into(), stop.into()),
            entry_id: self.proc_entries.get(proc_id).unwrap().clone(),
        })
    }

    // Use this function when the event entry for the critical path is actually the
    // critical path and we need to generate a link to the corresponding event entry
    fn generate_critical_link(&self, event: EventID, event_entry: &EventEntry) -> Field {
        let node = event.node_id();
        match event_entry.kind {
            EventEntryKind::UnknownEvent => {
                if event.is_barrier() {
                    // If you get here it means the user was running with
                    // -lg:prof_all_critical_arrivals
                    Field::String(format!(
                            "Unknown critical path barrier {:#x} created on node {}. Please load the logfile from at least one node that arrives on this barrier to start determining a critical path. You'll need to load the logs from all nodes that arrive on this barrier to determine a precise critical path. If you see this message and did not run with the -lg:prof_all_critical_arrivals flag then please report this case as it is likely a bug.",
                            event.0, node.0
                    ))
                } else {
                    Field::String(format!(
                            "Unknown critical path event {:#x} from node {}. Please load the logfile from that node to see it.",
                            event.0, node.0
                    ))
                }
            }
            EventEntryKind::TaskEvent => {
                let prof_uid = event_entry.creator.unwrap();
                if let Some(proc_id) = self.state.prof_uid_proc.get(&prof_uid) {
                    let trigger_time: ts::Timestamp = event_entry.trigger_time.unwrap().into();
                    let proc = self.state.procs.get(&proc_id).unwrap();
                    let entry = proc.find_entry(prof_uid).unwrap();
                    let op_name = entry.name(&self.state);
                    let proc_name = proc.name(&self.state);
                    Field::ItemLink(ItemLink {
                        item_uid: entry.base().prof_uid.into(),
                        title: format!(
                            "Completion of {} at {} on {}",
                            &op_name, trigger_time, proc_name
                        ),
                        interval: entry.time_range.into(),
                        entry_id: self.proc_entries.get(proc_id).unwrap().clone(),
                    })
                } else {
                    Field::String(format!(
                            "Critical path from a (meta-) task on node {}. Please load the logfile from that node to see it.",
                            node.0
                    ))
                }
            }
            EventEntryKind::FillEvent
            | EventEntryKind::CopyEvent
            | EventEntryKind::DepPartEvent => {
                let prof_uid = event_entry.creator.unwrap();
                if let Some(chan_id) = self.state.prof_uid_chan.get(&prof_uid) {
                    let trigger_time: ts::Timestamp = event_entry.trigger_time.unwrap().into();
                    let chan = self.state.chans.get(&chan_id).unwrap();
                    let entry = chan.find_entry(prof_uid).unwrap();
                    let name = entry.name(&self.state);
                    let chan_name = chan.name(&self.state);
                    Field::ItemLink(ItemLink {
                        item_uid: entry.base().prof_uid.into(),
                        title: format!(
                            "Completion of {} at {} in {}",
                            &name, trigger_time, chan_name
                        ),
                        interval: entry.time_range().into(),
                        entry_id: self.chan_entries.get(chan_id).unwrap().clone(),
                    })
                } else {
                    let kind = match event_entry.kind {
                        EventEntryKind::FillEvent => "fill",
                        EventEntryKind::CopyEvent => "copy",
                        EventEntryKind::DepPartEvent => "dependent partition operation",
                        _ => unreachable!(),
                    };
                    Field::String(format!(
                            "Critical path from a {} on node {}. Please load the logfile from that node to see it.",
                            kind, node.0
                    ))
                }
            }
            EventEntryKind::InstanceReady => {
                let prof_uid = event_entry.creator.unwrap();
                if let Some(mem_id) = self.state.insts.get(&prof_uid) {
                    // This means the critical path was the allocation of the instance and not
                    // the triggering of the precondition event
                    let mem = self.state.mems.get(&mem_id).unwrap();
                    let inst = mem.entry(prof_uid);
                    let ready_time: ts::Timestamp = inst.time_range.ready.unwrap().into();
                    let inst_name = inst.name(&self.state);
                    let mem_name = mem.name(&self.state);
                    Field::ItemLink(ItemLink {
                        item_uid: inst.base.prof_uid.into(),
                        title: format!(
                            "Allocation of {} at {} in {}",
                            &inst_name, ready_time, mem_name
                        ),
                        interval: inst.time_range.into(),
                        entry_id: self.mem_entries.get(mem_id).unwrap().clone(),
                    })
                } else {
                    Field::String(format!(
                            "Critical path from an instance creation on node {}. Please load the logfile from that node to see it.", node.0
                    ))
                }
            }
            EventEntryKind::InstanceDeletion => {
                let prof_uid = event_entry.creator.unwrap();
                if let Some(mem_id) = self.state.insts.get(&prof_uid) {
                    // This means the critical path was the deletion of the instance
                    let mem = self.state.mems.get(&mem_id).unwrap();
                    let inst = mem.entry(prof_uid);
                    let stop_time: ts::Timestamp = inst.time_range.stop.unwrap().into();
                    let inst_name = inst.name(&self.state);
                    let mem_name = mem.name(&self.state);
                    Field::ItemLink(ItemLink {
                        item_uid: inst.base.prof_uid.into(),
                        title: format!(
                            "Deletion of {} at {} in {}",
                            &inst_name, stop_time, mem_name
                        ),
                        interval: inst.time_range.into(),
                        entry_id: self.mem_entries.get(mem_id).unwrap().clone(),
                    })
                } else {
                    Field::String(format!(
                            "Critical path from an instance deletion on node {}. Please load the logfile from that node to see it.", node.0
                    ))
                }
            }
            EventEntryKind::ExternalHandshake => {
                assert!(event.is_barrier());
                let trigger_time = event_entry.trigger_time.unwrap();
                let trigger_ts: ts::Timestamp = trigger_time.into();
                Field::String(format!(
                    "External handshake on node {} at {}",
                    node.0, trigger_ts
                ))
            }
            // The rest of these only happen when the critical path is not along a chain
            // of events but when the (meta-) task producing the event is the last thing
            // to actually run to enable the execution
            EventEntryKind::MergeEvent
            | EventEntryKind::TriggerEvent
            | EventEntryKind::PoisonEvent
            | EventEntryKind::ArriveBarrier
            | EventEntryKind::ReservationAcquire
            | EventEntryKind::CompletionQueueEvent => {
                let prof_uid = event_entry.creator.unwrap();
                if let Some(proc_id) = self.state.prof_uid_proc.get(&prof_uid) {
                    let trigger_time = event_entry.trigger_time.unwrap();
                    let trigger_ts: ts::Timestamp = trigger_time.into();
                    let proc = self.state.procs.get(&proc_id).unwrap();
                    // This prof UID is just the fevent prof UID, find the actual executing entry
                    let entry = proc.find_executing_entry(prof_uid, trigger_time).unwrap();
                    let op_name = entry.name(&self.state);
                    let proc_name = proc.name(&self.state);
                    let kind = match event_entry.kind {
                        EventEntryKind::MergeEvent => "Event Merger",
                        EventEntryKind::TriggerEvent => "User Event Trigger",
                        EventEntryKind::PoisonEvent => "User Event Poisoned",
                        EventEntryKind::ArriveBarrier => "Barrier Arrival",
                        EventEntryKind::ReservationAcquire => "Reservation Acquire",
                        EventEntryKind::CompletionQueueEvent => "Completion Queue Non-Empty",
                        _ => unreachable!(),
                    };
                    Field::ItemLink(ItemLink {
                        item_uid: entry.base().prof_uid.into(),
                        title: format!(
                            "{} by {} at {} on {}",
                            kind, &op_name, trigger_ts, proc_name
                        ),
                        interval: ts::Interval::new(
                            entry.time_range.start.unwrap().into(),
                            trigger_ts,
                        ),
                        entry_id: self.proc_entries.get(proc_id).unwrap().clone(),
                    })
                } else {
                    let fevent = self.state.find_fevent(prof_uid);
                    let fevent_node = fevent.node_id();
                    let kind = match event_entry.kind {
                        EventEntryKind::MergeEvent => "n event merger",
                        EventEntryKind::TriggerEvent => " user event trigger",
                        EventEntryKind::PoisonEvent => " user event poison",
                        EventEntryKind::ArriveBarrier => " barrier arrival",
                        EventEntryKind::ReservationAcquire => " reservation acquire",
                        EventEntryKind::CompletionQueueEvent => " completion queue non-empty",
                        _ => unreachable!(),
                    };
                    if fevent_node == node {
                        // This is probably a bug if we get here because it means that we
                        // recorded something with an fevent that we don't recognize from the
                        // same node that should have produced this fevent
                        Field::String(format!(
                                "Could not find fevent {:#x} for a{} of event {:#x} on node {}. This is probably a bug in the Legion runtime logging not recording all fevents on a node. You could try running with '-lg:prof_self' to see if the fevent corresponds to a profiling meta-task, but most likely this is just a bug.", fevent.0, kind, event.0, fevent_node.0
                        ))
                    } else {
                        // This should only be a trigger/poison/arrive
                        // The others should produce events on the same node as where they are called
                        assert!(
                            event_entry.kind == EventEntryKind::TriggerEvent
                                || event_entry.kind == EventEntryKind::PoisonEvent
                                || event_entry.kind == EventEntryKind::ArriveBarrier
                        );
                        // In these cases we should load the file for the node with the fevent
                        Field::String(format!(
                                "Critical path from a{} on node {}. Please load the logfile from that node to see it.", 
                                kind, fevent_node.0
                        ))
                    }
                }
            }
        }
    }

    fn select_critical_color(&self, event_entry: &EventEntry) -> Option<Color32> {
        match event_entry.kind {
            // Uknown events get brown since we don't know them
            EventEntryKind::UnknownEvent => Some(Color32::BLUE),
            // Anything application related is good so normal color
            EventEntryKind::TaskEvent
            | EventEntryKind::FillEvent
            | EventEntryKind::CopyEvent
            | EventEntryKind::DepPartEvent
            | EventEntryKind::InstanceReady => None,
            // Anything else gets red because it wmeans we were slow hooking up the event graph
            _ => Some(Color32::RED),
        }
    }

    fn select_interval_color(&self, start: Timestamp, stop: Timestamp) -> Option<Color32> {
        if start <= stop {
            let diff = stop - start;
            // This is a bit of an arbitrary heuristic but we'll say anything less
            // than 100 us is good (normal), less than 1ms is ok (yellow), anything else red
            if diff < Timestamp::from_us(100) {
                None
            } else if diff < Timestamp::from_us(1000) {
                Some(Color32::GOLD)
            } else {
                Some(Color32::RED)
            }
        } else {
            // Negative intervals don't make sense so mark them as unclear
            Some(Color32::BLUE)
        }
    }

    fn select_deferred_color(&self, start: Timestamp, stop: Timestamp) -> Option<Color32> {
        assert!(start <= stop);
        // Deferred is the opposite of normal latencies, we want things to be deferred
        // for longer since it means that the runtime is ahead of execution
        let diff = stop - start;
        if diff < Timestamp::from_us(100) {
            Some(Color32::RED)
        } else if diff < Timestamp::from_us(1000) {
            Some(Color32::GOLD)
        } else {
            None
        }
    }

    fn parse_provenance(provenance: &str) -> Field {
        if let Ok(value) = serde_json::from_str(provenance) {
            if let serde_json::Value::Array(vec) = value {
                if let [_user, machine] = &*vec {
                    if let serde_json::Value::Object(map) = machine {
                        let mut result = Vec::new();
                        for (k, v) in map {
                            if let serde_json::Value::String(s) = v {
                                result.push(Field::String(format!("{}: {}", k, s)));
                            } else {
                                result.push(Field::String(format!("{}: {}", k, v)));
                            }
                        }
                        return Field::Vec(result);
                    }
                }
            }
        }
        Field::String(provenance.to_string())
    }

    fn generate_proc_slot_meta_tile(
        &self,
        entry_id: &EntryID,
        proc_id: ProcID,
        device: Option<DeviceKind>,
        tile_id: TileID,
        full: bool,
    ) -> SlotMetaTile {
        let proc = self.state.procs.get(&proc_id).unwrap();
        let mut m: Vec<Vec<ItemMeta>> = Vec::new();
        let items = self.build_items(proc, device, tile_id, full, Some(&mut m), |entry, info| {
            let ItemInfo {
                point_interval,
                expand,
            } = info;

            let name = entry.name(&self.state);
            let provenance = entry.provenance(&self.state);

            let mut fields = Vec::new();
            if expand {
                fields.push((self.fields.expanded_for_visibility, Field::Empty, None));
            }
            fields.push((self.fields.interval, Field::Interval(point_interval), None));
            if let Some(initiation_op) = entry.initiation_op {
                // FIXME: You might think that initiation_op is None rather than
                // needing this check with zero, but backwards compatibility is hard
                // You can remove this check once we stop needing to be compatible with Python
                if initiation_op != OpID::ZERO {
                    fields.push((
                        self.fields.operation,
                        self.generate_op_link(initiation_op),
                        None,
                    ));
                }
            }
            if let Some(op_id) = entry.op_id {
                let op = self.state.find_op(op_id).unwrap();
                let inst_set: BTreeSet<_> =
                    op.operation_inst_infos.iter().map(|i| i.inst_uid).collect();

                let insts: Vec<_> = inst_set
                    .iter()
                    .flat_map(|i| {
                        let result = self.generate_inst_link(*i, "");
                        conditional_assert!(
                            result.is_some(),
                            Config::all_logs(),
                            "Cannot find instance 0x{:x}",
                            i.0
                        );
                        result
                    })
                    .collect();
                fields.push((self.fields.insts, Field::Vec(insts), None));
            }
            if let Some(provenance) = provenance {
                fields.push((
                    self.fields.provenance,
                    Self::parse_provenance(provenance),
                    None,
                ));
            }
            if let Some(creator) = entry.creator() {
                // Check to see if these are function calls or tasks
                match entry.kind {
                    ProcEntryKind::MapperCall(..)
                    | ProcEntryKind::RuntimeCall(_)
                    | ProcEntryKind::ApplicationCall(_)
                    | ProcEntryKind::GPUKernel(_, _) => {
                        fields.push((self.fields.caller, self.generate_proc_link(creator), None));
                    }
                    _ => {
                        // Find the completion time of the previous entry that was executing
                        // on this processor so that we can check to see if it was why we
                        // were delayed from running
                        let mut has_critical = false;
                        let mut need_critical = self.state.has_critical_path_data();
                        // Check to see if we have a critical path event
                        if let Some(critical) = entry.critical() {
                            has_critical = true;
                            if let Some(event_entry) = self.state.find_critical_entry(critical) {
                                // Check to see if the critical entry happened before or after
                                // the creation of this processor entry
                                let creation_time = entry.creation_time();
                                // If we don't know about the critical event then we always
                                // report that as the critical path so the user is aware
                                // that there is a missing critical path
                                if event_entry.kind == EventEntryKind::UnknownEvent
                                    || creation_time <= event_entry.trigger_time.unwrap()
                                {
                                    // Created before critical event triggered so list both
                                    // fields separately since they wil be different
                                    fields.push((
                                        self.fields.creator,
                                        self.generate_creator_link(creator, creation_time),
                                        None,
                                    ));
                                    // Critical path is critical event triggering
                                    fields.push((
                                        self.fields.critical,
                                        self.generate_critical_link(critical, event_entry),
                                        self.select_critical_color(event_entry),
                                    ));
                                    if event_entry.kind != EventEntryKind::UnknownEvent {
                                        // Record the time it took Realm to propagate the event trigger
                                        let trigger_time = event_entry.trigger_time.unwrap();
                                        let ready_time = entry.time_range.ready.unwrap();
                                        fields.push((
                                            self.fields.trigger_time,
                                            Field::Interval(ts::Interval::new(
                                                trigger_time.into(),
                                                ready_time.into(),
                                            )),
                                            self.select_interval_color(trigger_time, ready_time),
                                        ));
                                    }
                                    need_critical = false;
                                }
                            }
                        }
                        if need_critical {
                            // Did not record the critical path yet
                            // Critical path is creation of the task
                            fields.push((
                                self.fields.critical,
                                self.generate_critical_creator_link(creator, entry.creation_time()),
                                // If we had a critical event but it triggered before we were made
                                // then that is very bad, otherwise we're fine
                                if has_critical {
                                    Some(Color32::RED)
                                } else {
                                    None
                                },
                            ));
                        }
                    }
                }
            } else {
                // No creator, still need to record the critical path if there is one
                match entry.kind {
                    ProcEntryKind::Task(..)
                    | ProcEntryKind::MetaTask(_)
                    | ProcEntryKind::ProfTask => {
                        if let Some(critical) = entry.critical() {
                            if let Some(event_entry) = self.state.find_critical_entry(critical) {
                                // Critical path is the critical event triggering
                                fields.push((
                                    self.fields.critical,
                                    self.generate_critical_link(critical, event_entry),
                                    self.select_critical_color(event_entry),
                                ));
                                if event_entry.kind != EventEntryKind::UnknownEvent {
                                    // Record the time it took Realm to propagate the event trigger
                                    let trigger_time = event_entry.trigger_time.unwrap();
                                    let ready_time = entry.time_range.ready.unwrap();
                                    fields.push((
                                        self.fields.trigger_time,
                                        Field::Interval(ts::Interval::new(
                                            trigger_time.into(),
                                            ready_time.into(),
                                        )),
                                        self.select_interval_color(trigger_time, ready_time),
                                    ));
                                }
                            }
                        }
                    }
                    _ => {}
                }
            }
            match entry.kind {
                ProcEntryKind::MapperCall(mapper_id, mapper_proc, _) => {
                    let mapper = self.state.mappers.get(&(mapper_id, mapper_proc)).unwrap();
                    fields.push((
                        self.fields.mapper,
                        Field::String(mapper.name.to_owned()),
                        None,
                    ));
                    if let Some(proc) = self.state.procs.get(&mapper_proc) {
                        let proc_name = format!(
                            "Node {} {:?} {}",
                            mapper_proc.node_id().0,
                            proc.kind,
                            mapper_proc.proc_in_node()
                        );
                        fields.push((self.fields.mapper_proc, Field::String(proc_name), None));
                    } else {
                        let proc_name = format!("Node {}", mapper_proc.node_id().0);
                        fields.push((self.fields.mapper_proc, Field::String(proc_name), None));
                    }
                }
                _ => {}
            }
            if let Some(ready) = entry.time_range.ready {
                if let Some(create) = entry.time_range.create {
                    if let Some(spawn) = entry.time_range.spawn {
                        fields.push((
                            self.fields.message_latency,
                            Field::Interval(ts::Interval::new(spawn.into(), create.into())),
                            self.select_interval_color(spawn, create),
                        ));
                    }
                    fields.push((
                        self.fields.deferred_time,
                        Field::Interval(ts::Interval::new(create.into(), ready.into())),
                        // Check to see if this entry is an application task or a meta-task
                        // If an application task we want it to be deferred for a long time
                        // Runtime meta-tasks should be deferred for a shorter time
                        if entry.is_meta() {
                            self.select_interval_color(create, ready)
                        } else {
                            self.select_deferred_color(create, ready)
                        },
                    ));
                }
                if let Some(start) = entry.time_range.start {
                    fields.push((
                        self.fields.delayed_time,
                        Field::Interval(ts::Interval::new(ready.into(), start.into())),
                        self.select_interval_color(ready, start),
                    ));
                    // See if there was something previously executing that delayed us
                    if let Some((previous, start_time, stop_time)) =
                        proc.find_previous_executing_entry(ready, start, device)
                    {
                        fields.push((
                            self.fields.previous_executing,
                            self.generate_previous_executing_link(previous, start_time, stop_time),
                            None,
                        ));
                        fields.push((
                            self.fields.scheduling_overhead,
                            Field::Interval(ts::Interval::new(stop_time.into(), start.into())),
                            self.select_interval_color(stop_time, start),
                        ));
                    }
                }
            }
            ItemMeta {
                item_uid: entry.base().prof_uid.into(),
                title: name,
                original_interval: point_interval,
                fields,
            }
        });
        assert_eq!(items.len(), m.len());
        for (item_row, item_meta_row) in items.iter().zip(m.iter()) {
            assert_eq!(item_row.len(), item_meta_row.len());
        }
        SlotMetaTile {
            entry_id: entry_id.clone(),
            tile_id,
            data: SlotMetaTileData { items: m },
        }
    }

    fn generate_mem_slot_tile(
        &self,
        entry_id: &EntryID,
        mem_id: MemID,
        tile_id: TileID,
        full: bool,
    ) -> SlotTile {
        let mem = self.state.mems.get(&mem_id).unwrap();
        let items = self.build_items(mem, None, tile_id, full, None, |_, _| unreachable!());
        SlotTile {
            entry_id: entry_id.clone(),
            tile_id,
            data: SlotTileData { items },
        }
    }

    fn generate_inst_regions(
        &self,
        inst: &Inst,
        result: &mut Vec<(FieldID, Field, Option<Color32>)>,
    ) {
        for (ispace_id, fspace_id) in inst.ispace_ids.iter().zip(inst.fspace_ids.iter()) {
            let ispace = format!("{}", ISpacePretty(*ispace_id, &self.state),);
            result.push((self.fields.inst_ispace, Field::String(ispace), None));

            let fspace = self.state.field_spaces.get(&fspace_id).unwrap();
            let fspace_name = format!("{}", FSpaceShort(&fspace));
            result.push((self.fields.inst_fspace, Field::String(fspace_name), None));

            let fields = format!("{}", FieldsPretty(&fspace, inst));
            result.push((self.fields.inst_fields, Field::String(fields), None));
        }
    }

    fn generate_inst_layout(
        &self,
        inst: &Inst,
        result: &mut Vec<(FieldID, Field, Option<Color32>)>,
    ) {
        let layout = format!("{}", DimOrderPretty(inst, false));
        result.push((self.fields.inst_layout, Field::String(layout), None));
    }

    fn generate_inst_size(&self, inst: &Inst, result: &mut Vec<(FieldID, Field, Option<Color32>)>) {
        let size = format!("{}", SizePretty(inst.size.unwrap()));
        result.push((self.fields.size, Field::String(size), None));
    }

    fn generate_mem_slot_meta_tile(
        &self,
        entry_id: &EntryID,
        mem_id: MemID,
        tile_id: TileID,
        full: bool,
    ) -> SlotMetaTile {
        let mem = self.state.mems.get(&mem_id).unwrap();
        let mut m: Vec<Vec<ItemMeta>> = Vec::new();
        let items = self.build_items(mem, None, tile_id, full, Some(&mut m), |entry, info| {
            let ItemInfo {
                point_interval,
                expand,
            } = info;

            let name = format!("Instance {}", InstShort(entry));
            let provenance = entry.provenance(&self.state);

            let mut fields = Vec::new();
            if expand {
                fields.push((self.fields.expanded_for_visibility, Field::Empty, None));
            }
            fields.push((self.fields.interval, Field::Interval(point_interval), None));
            self.generate_inst_regions(entry, &mut fields);
            self.generate_inst_layout(entry, &mut fields);
            self.generate_inst_size(entry, &mut fields);
            if let Some(initiation_op) = entry.initiation() {
                // FIXME: You might think that initiation_op is None rather than
                // needing this check with zero, but backwards compatibility is hard
                // You can remove this check once we stop needing to be compatible with Python
                if initiation_op != OpID::ZERO {
                    fields.push((
                        self.fields.operation,
                        self.generate_op_link(initiation_op),
                        None,
                    ));
                }
            }
            if let Some(provenance) = provenance {
                fields.push((
                    self.fields.provenance,
                    Self::parse_provenance(provenance),
                    None,
                ));
            }
            // Do the critical path analysis for this instance
            // There are three things that can delay an instance creation
            // 1. The precondition event can be slow to trigger
            // 2. The caller task can be slow to create it
            // 3. We might need to wait for space in the memory to be freed for it to be ready
            let mut need_critical = self.state.has_critical_path_data();
            if let Some(critical) = entry.critical() {
                if let Some(event_entry) = self.state.find_critical_entry(critical) {
                    // Check to see if the critical entry happened before or after
                    // the creation of this processor entry
                    let creation_time = entry.creation_time();
                    // If we don't know about the critical event then we always want to
                    // report that as the critical event so the user is aware of it
                    if event_entry.kind == EventEntryKind::UnknownEvent
                        || creation_time <= event_entry.trigger_time.unwrap()
                    {
                        // Created before critical event triggered so list both
                        // fields separately since they wil be different
                        if let Some(creator) = entry.creator() {
                            let creation_time = entry.time_range.create.unwrap();
                            fields.push((
                                self.fields.creator,
                                self.generate_creator_link(creator, creation_time),
                                None,
                            ));
                        }
                        fields.push((
                            self.fields.critical,
                            self.generate_critical_link(critical, event_entry),
                            self.select_critical_color(event_entry),
                        ));
                        if event_entry.kind != EventEntryKind::UnknownEvent {
                            // Record the time it took Realm to propagate the event trigger
                            let trigger_time = event_entry.trigger_time.unwrap();
                            let ready_time = entry.time_range.ready.unwrap();
                            fields.push((
                                self.fields.trigger_time,
                                Field::Interval(ts::Interval::new(
                                    trigger_time.into(),
                                    ready_time.into(),
                                )),
                                self.select_interval_color(trigger_time, ready_time),
                            ));
                        }
                        need_critical = false;
                    }
                }
            }
            if need_critical {
                // No critical event so check conditions 2 and 3
                let creation_time = entry.time_range.create.unwrap();
                if entry.allocated_immediately() {
                    // Critical path is the creator
                    if let Some(creator) = entry.creator() {
                        fields.push((
                            self.fields.critical,
                            self.generate_critical_creator_link(creator, creation_time),
                            None,
                        ));
                    } else {
                        let creation_ts: ts::Timestamp = creation_time.into();
                        fields.push((
                            self.fields.critical,
                            Field::String(format!("Unknown creator at {}", creation_ts)),
                            Some(Color32::BLUE),
                        ));
                    }
                } else {
                    // Critical path is waiting for other instances to be deleted
                    let ready_time = entry.time_range.ready.unwrap();
                    let ready_ts: ts::Timestamp = ready_time.into();
                    fields.push((
                        self.fields.critical,
                        Field::String(format!(
                            "Waiting for deallocation of other instances until {}",
                            ready_ts
                        )),
                        Some(Color32::GOLD),
                    ));
                    // Record the deferred time here for how long we waited for
                    // the instance to be ready
                    fields.push((
                        self.fields.deferred_time,
                        Field::Interval(ts::Interval::new(creation_time.into(), ready_ts)),
                        self.select_interval_color(creation_time, ready_time),
                    ));
                    // Still need to record the creator
                    if let Some(creator) = entry.creator() {
                        let creation_time = entry.time_range.create.unwrap();
                        fields.push((
                            self.fields.creator,
                            self.generate_creator_link(creator, creation_time),
                            None,
                        ));
                    }
                }
            }

            ItemMeta {
                item_uid: entry.base().prof_uid.into(),
                title: name,
                original_interval: point_interval,
                fields,
            }
        });
        assert_eq!(items.len(), m.len());
        for (item_row, item_meta_row) in items.iter().zip(m.iter()) {
            assert_eq!(item_row.len(), item_meta_row.len());
        }
        SlotMetaTile {
            entry_id: entry_id.clone(),
            tile_id,
            data: SlotMetaTileData { items: m },
        }
    }

    fn generate_chan_slot_tile(
        &self,
        entry_id: &EntryID,
        chan_id: ChanID,
        tile_id: TileID,
        full: bool,
    ) -> SlotTile {
        let chan = self.state.chans.get(&chan_id).unwrap();
        let items = self.build_items(chan, None, tile_id, full, None, |_, _| unreachable!());
        SlotTile {
            entry_id: entry_id.clone(),
            tile_id,
            data: SlotTileData { items },
        }
    }

    fn generate_copy_reqs(&self, copy: &Copy, result_reqs: &mut Vec<Field>) {
        let groups = copy.copy_inst_infos.linear_group_by(|a, b| {
            a.src_inst_uid == b.src_inst_uid
                && a.dst_inst_uid == b.dst_inst_uid
                && a.num_hops == b.num_hops
        });
        let mut i = 0;
        for group in groups {
            let req_nums = if group.len() == 1 {
                format!("Requirement {}", i)
            } else {
                format!("Requirements {}-{}", i, i + group.len() - 1)
            };
            result_reqs.push(Field::String(req_nums));

            let CopyInstInfo {
                src_inst_uid,
                dst_inst_uid,
                num_hops,
                ..
            } = group[0];

            let src_inst = if let Some(src_uid) = src_inst_uid {
                self.state.find_inst(src_uid)
            } else {
                None
            };
            let dst_inst = if let Some(dst_uid) = dst_inst_uid {
                self.state.find_inst(dst_uid)
            } else {
                None
            };

            let src_fids = group.iter().map(|x| x.src_fid).collect();
            let src_fields = format!(
                "Fields: {}",
                ChanEntryFieldsPretty(src_inst, &src_fids, &self.state)
            );

            let dst_fids = group.iter().map(|x| x.dst_fid).collect();
            let dst_fields = format!(
                "Fields: {}",
                ChanEntryFieldsPretty(dst_inst, &dst_fids, &self.state)
            );

            match (src_inst_uid, dst_inst_uid) {
                (None, None) => unreachable!(),
                (None, Some(dst_uid)) => {
                    let prefix = "Scatter: destination indirect instance ";
                    if let Some(dst) = self.generate_inst_link(dst_uid, prefix) {
                        result_reqs.push(dst);
                    } else {
                        result_reqs.push(Field::String(format!("{}<unknown instance>", prefix)));
                    }
                    result_reqs.push(Field::String(dst_fields));
                }
                (Some(src_uid), None) => {
                    let prefix = "Gather: source indirect instance ";
                    if let Some(src) = self.generate_inst_link(src_uid, prefix) {
                        result_reqs.push(src);
                    } else {
                        result_reqs.push(Field::String(format!("{}<unknown instance>", prefix)));
                    }
                    result_reqs.push(Field::String(src_fields));
                }
                (Some(src_uid), Some(dst_uid)) => {
                    let prefix = "Source: ";
                    if let Some(src) = self.generate_inst_link(src_uid, prefix) {
                        result_reqs.push(src);
                    } else {
                        result_reqs.push(Field::String(format!("{}<unknown instance>", prefix)));
                    }
                    result_reqs.push(Field::String(src_fields));

                    let prefix = "Destination: ";
                    if let Some(dst) = self.generate_inst_link(dst_uid, prefix) {
                        result_reqs.push(dst);
                    } else {
                        result_reqs.push(Field::String(format!("{}<unknown instance>", prefix)));
                    }
                    result_reqs.push(Field::String(dst_fields));
                }
            }

            result_reqs.push(Field::String(format!("Number of Hops: {}", num_hops)));

            i += group.len();
        }
    }

    fn generate_fill_reqs(&self, fill: &Fill, result_reqs: &mut Vec<Field>) {
        let groups = fill
            .fill_inst_infos
            .linear_group_by(|a, b| a.dst_inst_uid == b.dst_inst_uid);
        let mut i = 0;
        for group in groups {
            let req_nums = if group.len() == 1 {
                format!("Requirement {}", i)
            } else {
                format!("Requirements {}-{}", i, i + group.len() - 1)
            };
            result_reqs.push(Field::String(req_nums));

            let FillInstInfo { dst_inst_uid, .. } = group[0];

            let dst_inst = self.state.find_inst(dst_inst_uid);

            let dst_fids = group.iter().map(|x| x.fid).collect();
            let dst_fields = format!(
                "Fields: {}",
                ChanEntryFieldsPretty(dst_inst, &dst_fids, &self.state)
            );

            let prefix = "Destination: ";
            if let Some(dst) = self.generate_inst_link(dst_inst_uid, prefix) {
                result_reqs.push(dst);
            } else {
                result_reqs.push(Field::String(format!("{}<unknown instance>", prefix)));
            }
            result_reqs.push(Field::String(dst_fields));

            i += group.len();
        }
    }

    fn generate_chan_reqs(
        &self,
        entry: &ChanEntry,
        result: &mut Vec<(FieldID, Field, Option<Color32>)>,
    ) {
        let mut result_reqs = Vec::new();
        match entry {
            ChanEntry::Copy(copy) => {
                self.generate_copy_reqs(copy, &mut result_reqs);
            }
            ChanEntry::Fill(fill) => {
                self.generate_fill_reqs(fill, &mut result_reqs);
            }
            ChanEntry::DepPart(_) => {}
        }
        result.push((self.fields.chan_reqs, Field::Vec(result_reqs), None));
    }

    fn generate_chan_size(
        &self,
        entry: &ChanEntry,
        result: &mut Vec<(FieldID, Field, Option<Color32>)>,
    ) {
        let size = match entry {
            ChanEntry::Copy(copy) => copy.size,
            ChanEntry::Fill(fill) => fill.size,
            ChanEntry::DepPart(_) => return,
        };
        let size = format!("{}", SizePretty(size));
        result.push((self.fields.size, Field::String(size), None));
    }

    fn generate_chan_slot_meta_tile(
        &self,
        entry_id: &EntryID,
        chan_id: ChanID,
        tile_id: TileID,
        full: bool,
    ) -> SlotMetaTile {
        let chan = self.state.chans.get(&chan_id).unwrap();
        let mut m: Vec<Vec<ItemMeta>> = Vec::new();
        let items = self.build_items(chan, None, tile_id, full, Some(&mut m), |entry, info| {
            let ItemInfo {
                point_interval,
                expand,
            } = info;

            let name = format!("{}", ChanEntryShort(entry));
            let provenance = entry.provenance(&self.state);

            let mut fields = Vec::new();
            if expand {
                fields.push((self.fields.expanded_for_visibility, Field::Empty, None));
            }
            fields.push((self.fields.interval, Field::Interval(point_interval), None));
            self.generate_chan_reqs(entry, &mut fields);
            self.generate_chan_size(entry, &mut fields);
            if let Some(initiation_op) = entry.initiation() {
                // FIXME: You might think that initiation_op is None rather than
                // needing this check with zero, but backwards compatibility is hard
                // You can remove this check once we stop needing to be compatible with Python
                if initiation_op != OpID::ZERO {
                    fields.push((
                        self.fields.operation,
                        self.generate_op_link(initiation_op),
                        None,
                    ));
                }
            }
            if let Some(provenance) = provenance {
                fields.push((
                    self.fields.provenance,
                    Self::parse_provenance(provenance),
                    None,
                ));
            }
            let time_range = entry.time_range();
            if let Some(creator) = entry.creator() {
                if let Some(critical) = entry.critical() {
                    if let Some(event_entry) = self.state.find_critical_entry(critical) {
                        // Check to see if the critical entry happened before or after
                        // the creation of this processor entry
                        let creation_time = entry.creation_time();
                        // If we don't know about the critical event then we always
                        // report that as the critical path so the user is aware
                        // that there is a missing critical path
                        if event_entry.kind != EventEntryKind::UnknownEvent
                            && event_entry.trigger_time.unwrap() < creation_time
                        {
                            // Created after critical event triggered
                            fields.push((
                                self.fields.critical,
                                self.generate_critical_creator_link(creator, creation_time),
                                Some(Color32::RED),
                            ));
                        } else {
                            // Created before critical event triggered so list both
                            // fields separately since they will be different
                            fields.push((
                                self.fields.creator,
                                self.generate_creator_link(creator, creation_time),
                                None,
                            ));
                            fields.push((
                                self.fields.critical,
                                self.generate_critical_link(critical, event_entry),
                                self.select_critical_color(event_entry),
                            ));
                            if event_entry.kind != EventEntryKind::UnknownEvent {
                                // Record the time it took Realm to propagate the event trigger
                                let trigger_time = event_entry.trigger_time.unwrap();
                                let ready_time = time_range.ready.unwrap();
                                fields.push((
                                    self.fields.trigger_time,
                                    Field::Interval(ts::Interval::new(
                                        trigger_time.into(),
                                        ready_time.into(),
                                    )),
                                    self.select_interval_color(trigger_time, ready_time),
                                ));
                            }
                        }
                    } else {
                        // No critical entry so assume creation was the critical path
                        fields.push((
                            self.fields.critical,
                            self.generate_critical_creator_link(creator, entry.creation_time()),
                            None,
                        ));
                    }
                } else {
                    // No critical event so the creation was definitely the critical path
                    fields.push((
                        self.fields.critical,
                        self.generate_critical_creator_link(creator, entry.creation_time()),
                        None,
                    ));
                }
            } else if let Some(critical) = entry.critical() {
                // No creator so if we have critical entry that is the critical path
                if let Some(event_entry) = self.state.find_critical_entry(critical) {
                    fields.push((
                        self.fields.critical,
                        self.generate_critical_link(critical, event_entry),
                        self.select_critical_color(event_entry),
                    ));
                    if event_entry.kind != EventEntryKind::UnknownEvent {
                        let trigger_time = event_entry.trigger_time.unwrap();
                        let ready_time = time_range.ready.unwrap();
                        // Record the time it took Realm to propagate the event trigger
                        fields.push((
                            self.fields.trigger_time,
                            Field::Interval(ts::Interval::new(
                                trigger_time.into(),
                                ready_time.into(),
                            )),
                            self.select_interval_color(trigger_time, ready_time),
                        ));
                    }
                }
            }
            if let Some(ready) = time_range.ready {
                if let Some(create) = time_range.create {
                    fields.push((
                        self.fields.deferred_time,
                        Field::Interval(ts::Interval::new(create.into(), ready.into())),
                        self.select_deferred_color(create, ready),
                    ));
                }
                if let Some(start) = time_range.start {
                    fields.push((
                        self.fields.delayed_time,
                        Field::Interval(ts::Interval::new(ready.into(), start.into())),
                        self.select_interval_color(ready, start),
                    ));
                }
            }
            ItemMeta {
                item_uid: entry.base().prof_uid.into(),
                title: name,
                original_interval: point_interval,
                fields,
            }
        });
        assert_eq!(items.len(), m.len());
        for (item_row, item_meta_row) in items.iter().zip(m.iter()) {
            assert_eq!(item_row.len(), item_meta_row.len());
        }
        SlotMetaTile {
            entry_id: entry_id.clone(),
            tile_id,
            data: SlotMetaTileData { items: m },
        }
    }

    fn interval(&self) -> ts::Interval {
        let last_time = self.state.last_time;
        // Add a bit to the end of the timeline to make it more visible
        let last_time = last_time + Timestamp::from_ns(last_time.to_ns() / 200);
        ts::Interval::new(ts::Timestamp(0), last_time.into())
    }

    fn generate_warning_message(&self) -> Option<String> {
        if !self.state.runtime_config.any() {
            return None;
        }
        Some(format!(
            "This profile was generated with {}. Extreme performance degradation may occur.",
            self.state.runtime_config
        ))
    }
}

impl DataSource for StateDataSource {
    fn fetch_description(&self) -> DataSourceDescription {
        DataSourceDescription {
            source_locator: self.state.source_locator.clone(),
        }
    }

    fn fetch_info(&self) -> DataSourceInfo {
        DataSourceInfo {
            entry_info: self.info.clone(),
            interval: self.interval(),
            tile_set: TileSet::default(),
            field_schema: self.field_schema.clone(),
            warning_message: self.generate_warning_message(),
        }
    }

    fn fetch_summary_tile(&self, entry_id: &EntryID, tile_id: TileID, full: bool) -> SummaryTile {
        // Pick this number to be approximately the number of pixels we expect
        // the user to have on their screen. If this is a full tile, increase
        // this so that we get more resolution when zoomed in.
        let samples = if full { 4_000 } else { 800 };

        let step_utilization = self.generate_step_utilization(entry_id);

        let utilization = Self::compute_sample_utilization(&step_utilization, tile_id.0, samples);

        SummaryTile {
            entry_id: entry_id.clone(),
            tile_id,
            data: SummaryTileData { utilization },
        }
    }

    fn fetch_slot_tile(&self, entry_id: &EntryID, tile_id: TileID, full: bool) -> SlotTile {
        let entry = self.entry_map.get(entry_id).unwrap();
        match entry {
            EntryKind::Proc(proc_id, device) => {
                self.generate_proc_slot_tile(entry_id, *proc_id, *device, tile_id, full)
            }
            EntryKind::Mem(mem_id) => self.generate_mem_slot_tile(entry_id, *mem_id, tile_id, full),
            EntryKind::Chan(chan_id) | EntryKind::DepPart(chan_id) => {
                self.generate_chan_slot_tile(entry_id, *chan_id, tile_id, full)
            }
            _ => unreachable!(),
        }
    }

    fn fetch_slot_meta_tile(
        &self,
        entry_id: &EntryID,
        tile_id: TileID,
        full: bool,
    ) -> SlotMetaTile {
        let entry = self.entry_map.get(entry_id).unwrap();
        match entry {
            EntryKind::Proc(proc_id, device) => {
                self.generate_proc_slot_meta_tile(entry_id, *proc_id, *device, tile_id, full)
            }
            EntryKind::Mem(mem_id) => {
                self.generate_mem_slot_meta_tile(entry_id, *mem_id, tile_id, full)
            }
            EntryKind::Chan(chan_id) | EntryKind::DepPart(chan_id) => {
                self.generate_chan_slot_meta_tile(entry_id, *chan_id, tile_id, full)
            }
            _ => unreachable!(),
        }
    }
}
