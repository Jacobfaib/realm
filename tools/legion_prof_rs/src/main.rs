use std::collections::BTreeSet;
use std::ffi::OsString;
use std::io;
#[cfg(feature = "client")]
use std::path::Path;
use std::path::PathBuf;

use clap::{Args, Parser, Subcommand};

use rayon::prelude::*;

#[cfg(feature = "client")]
use url::Url;

#[cfg(feature = "client")]
use legion_prof_viewer::{
    app, deferred_data::DeferredDataSource, file_data::FileDataSource,
    http::client::HTTPClientDataSource, parallel_data::ParallelDeferredDataSource,
};

#[cfg(feature = "archiver")]
use legion_prof::backend::archiver;
#[cfg(feature = "duckdb")]
use legion_prof::backend::duckdb;
#[cfg(feature = "nvtxw")]
use legion_prof::backend::nvtxw;
#[cfg(feature = "server")]
use legion_prof::backend::server;
#[cfg(feature = "viewer")]
use legion_prof::backend::viewer;
use legion_prof::backend::{analyze, dump, trace_viewer, visualize};
use legion_prof::serialize::deserialize;
use legion_prof::state::{Config, NodeID, State, Timestamp};

#[derive(Debug, Clone, Args)]
struct ParserArgs {
    #[arg(required = true, help = "input Legion Prof log filenames")]
    filenames: Vec<OsString>,

    #[arg(long, help = "start time in microseconds to trim the profile")]
    start_trim: Option<u64>,

    #[arg(long, help = "stop time in microseconds to trim the profile")]
    stop_trim: Option<u64>,

    #[arg(
        long,
        default_value_t = 1000.0,
        help = "threshold for warning about message latencies in microseconds"
    )]
    message_threshold: f64,

    #[arg(
        long,
        default_value_t = 5.0,
        help = "perentage of messages that must be over the threshold to trigger a warning"
    )]
    message_percentage: f64,

    #[arg(
        long,
        default_value_t = 0,
        help = "minimum threshold (in microseconds) for visualizing function calls"
    )]
    call_threshold: u64,

    #[arg(long, help = "a list of nodes that will be visualized")]
    nodes: Option<String>,

    #[arg(
        long,
        hide = true,
        help = "parse all log files, even when a subset of nodes are being shown (uses more memory)"
    )]
    no_filter_input: bool,

    #[arg(short, long, help = "print verbose profiling information")]
    verbose: bool,
}

#[derive(Debug, Clone, Args)]
struct OutputArgs {
    #[arg(
        short,
        long,
        default_value = "legion_prof",
        help = "output directory pathname"
    )]
    output: OsString,

    #[arg(short, long, help = "overwrite output directory if it exists")]
    force: bool,
}

#[derive(Debug, Clone, Subcommand)]
enum Commands {
    #[command(about = "save an archive of the profile for sharing")]
    Archive {
        #[command(flatten)]
        args: ParserArgs,

        #[command(flatten)]
        out: OutputArgs,

        #[arg(long, default_value_t = 4, help = "number of zoom levels to archive")]
        levels: u32,

        #[arg(long, default_value_t = 4, help = "branch factor for archive")]
        branch_factor: u64,

        #[arg(
            long,
            default_value_t = 10,
            help = "zstd compression factor for archive"
        )]
        zstd_compression: i32,
    },
    #[command(about = "connect viewer to an HTTP server or (local/remote) archive")]
    Attach {
        #[arg(required = true, help = "URL(s) or path(s) to attach to")]
        args: Vec<OsString>,
    },
    #[command(name = "duckdb", about = "save profile to DuckDB database")]
    DuckDB {
        #[command(flatten)]
        args: ParserArgs,

        #[command(flatten)]
        out: OutputArgs,
    },
    #[command(about = "dump parsed log files in a JSON format")]
    Dump {
        #[command(flatten)]
        args: ParserArgs,
    },
    #[command(about = "dump a legacy format profile for sharing")]
    Legacy {
        #[command(flatten)]
        args: ParserArgs,

        #[command(flatten)]
        out: OutputArgs,
    },
    #[command(about = "process data through NVTXW for NVIDIA Nsight Systems")]
    NVTXW {
        #[command(flatten)]
        args: ParserArgs,

        #[arg(long, help = "path to NVTXW backend implementation")]
        backend: Option<OsString>,

        #[arg(long, help = "output nsys-rep filename")]
        output: OsString,

        #[arg(short, long, help = "overwrite output file if it exists")]
        force: bool,

        #[arg(long, help = "input nsys-rep filename to merge with Legion Prof data")]
        merge: Option<OsString>,
    },
    #[command(about = "start profile HTTP server")]
    Serve {
        #[command(flatten)]
        args: ParserArgs,

        #[arg(
            long,
            default_value = "127.0.0.1",
            help = "host to bind for HTTP server"
        )]
        host: String,

        #[arg(long, default_value_t = 8080, help = "port to bind for HTTP server")]
        port: u16,
    },
    #[command(about = "start interactive profile viewer")]
    View {
        #[command(flatten)]
        args: ParserArgs,
    },
    #[command(about = "print statistics")]
    Statistics {
        #[command(flatten)]
        args: ParserArgs,
    },
    #[command(about = "emit JSON for Google Trace Viewer")]
    Trace {
        #[command(flatten)]
        args: ParserArgs,

        #[command(flatten)]
        out: OutputArgs,
    },
}

#[derive(Debug, Clone, Parser)]
#[command(author, version, about, long_about = None)]
struct Cli {
    #[command(subcommand)]
    command: Commands,
}

fn main() -> io::Result<()> {
    env_logger::init();

    let cli = Cli::parse();

    match cli.command {
        Commands::Archive { .. } => {
            #[cfg(not(feature = "archiver"))]
            panic!(
                "Legion Prof was not built with the \"archiver\" feature. \
                 Rebuild with --features=archiver to enable."
            );
        }
        Commands::Attach { .. } => {
            #[cfg(not(feature = "client"))]
            panic!(
                "Legion Prof was not built with the \"client\" feature. \
                 Rebuild with --features=client to enable."
            );
        }
        Commands::DuckDB { .. } => {
            #[cfg(not(feature = "duckdb"))]
            panic!(
                "Legion Prof was not built with the \"duckdb\" feature. \
                 Rebuild with --features=duckdb to enable."
            );
        }
        Commands::NVTXW { .. } => {
            #[cfg(not(feature = "nvtxw"))]
            panic!(
                "Legion Prof was not built with the \"nvtxw\" feature. \
                 Rebuild with --features=nvtxw to enable."
            );
        }
        Commands::Serve { .. } => {
            #[cfg(not(feature = "server"))]
            panic!(
                "Legion Prof was not built with the \"server\" feature. \
                 Rebuild with --features=server to enable."
            );
        }
        Commands::View { .. } => {
            #[cfg(not(feature = "viewer"))]
            panic!(
                "Legion Prof was not built with the \"viewer\" feature. \
                 Rebuild with --features=viewer to enable."
            );
        }
        _ => {}
    }

    match cli.command {
        Commands::Attach { args } => {
            #[cfg(feature = "client")]
            {
                fn http_ds(url: Url) -> Box<dyn DeferredDataSource> {
                    Box::new(HTTPClientDataSource::new(url))
                }

                fn file_ds(path: impl AsRef<Path>) -> Box<dyn DeferredDataSource> {
                    Box::new(ParallelDeferredDataSource::new(FileDataSource::new(path)))
                }

                let data_sources: Vec<_> = args
                    .into_iter()
                    .map(|arg| {
                        arg.into_string()
                            .map(|s| Url::parse(&s).map(http_ds).unwrap_or_else(|_| {
                                println!("The argument '{}' does not appear to be a valid URL. Attempting to open it as a local file...", &s);
                                file_ds(&s)
                            }))
                            .unwrap_or_else(file_ds)
                    })
                    .collect();
                app::start(data_sources);
            }
            return Ok(());
        }
        _ => {}
    }

    let args = match cli.command {
        Commands::Archive { ref args, .. }
        | Commands::Dump { ref args, .. }
        | Commands::Legacy { ref args, .. }
        | Commands::DuckDB { ref args, .. }
        | Commands::NVTXW { ref args, .. }
        | Commands::View { ref args, .. }
        | Commands::Serve { ref args, .. }
        | Commands::Statistics { ref args, .. }
        | Commands::Trace { ref args, .. } => args,
        Commands::Attach { .. } => unreachable!(),
    };

    let start_trim = args.start_trim.map(Timestamp::from_us);
    let stop_trim = args.stop_trim.map(Timestamp::from_us);
    let message_threshold = args.message_threshold;
    let message_percentage = args.message_percentage;

    let mut node_list: Vec<NodeID> = Vec::new();
    let mut filter_input = false;
    if let Some(nodes_str) = &args.nodes {
        node_list = nodes_str
            .split(",")
            .flat_map(|x| {
                let splits: Vec<_> = x
                    .splitn(2, "-")
                    .map(|x| x.parse::<u64>().unwrap())
                    .collect();
                if splits.len() == 2 {
                    (splits[0]..=splits[1]).into_iter().map(NodeID)
                } else {
                    (splits[0]..=splits[0]).into_iter().map(NodeID)
                }
            })
            .collect();
        filter_input = !args.no_filter_input;
    }

    let records: Result<Vec<_>, _> = args
        .filenames
        .par_iter()
        .map(|filename| {
            println!("Reading log file {:?}...", filename);
            deserialize(filename, &node_list, filter_input)
        })
        .collect();
    match cli.command {
        Commands::Dump { .. } => {
            for record in records? {
                dump::dump_record(&record)?;
            }
            return Ok(());
        }
        _ => {}
    }

    let mut state = State::default();

    let paths: Vec<_> = args.filenames.iter().map(PathBuf::from).collect();

    let mut unique_paths = BTreeSet::<String>::new();
    for p in paths {
        if let Some(base) = p.parent() {
            unique_paths.insert(base.to_string_lossy().to_string());
        }
    }

    state.source_locator.extend(unique_paths.into_iter());

    state.visible_nodes = node_list;
    if filter_input {
        println!("Filtering profiles to nodes: {:?}", state.visible_nodes);
    }
    for record in records? {
        println!("Matched {} objects", record.len());
        state.process_records(&record, Timestamp::from_us(args.call_threshold));
    }

    if !state.complete_parse() {
        println!("Nothing to do");
        return Ok(());
    }

    let mut have_alllogs = true;
    // if number of files
    let num_nodes: usize = state.num_nodes.try_into().unwrap();
    if num_nodes > args.filenames.len() {
        println!(
            "Warning: This run involved {:?} nodes, but only {:?} log files were provided. If --verbose is enabled, subsequent warnings may not indicate a true error.",
            num_nodes,
            args.filenames.len()
        );
        have_alllogs = false;
    }

    // check if subnodes is enabled and filter input is true
    if state.visible_nodes.len() < num_nodes && filter_input {
        println!(
            "Warning: This run involved {:?} nodes, but only {:?} log files were used. If --verbose ie enabled, subsequent warnings may not indicate a true error.",
            num_nodes,
            state.visible_nodes.len()
        );
        have_alllogs = false;
    }

    Config::set_config(filter_input, args.verbose, have_alllogs);

    state.trim_time_range(start_trim, stop_trim);
    println!("Sorting time ranges");
    state.sort_time_range();
    state.check_message_latencies(message_threshold, message_percentage);
    state.filter_output();
    println!("Calculating critical paths");
    state.compute_critical_paths();

    match cli.command {
        Commands::Archive {
            out,
            levels,
            branch_factor,
            zstd_compression,
            ..
        } => {
            #[cfg(feature = "archiver")]
            {
                state.stack_time_points();
                state.assign_colors();
                archiver::write(
                    state,
                    levels,
                    branch_factor,
                    out.output,
                    out.force,
                    zstd_compression,
                )?;
            }
        }
        Commands::DuckDB { out, .. } => {
            #[cfg(feature = "duckdb")]
            {
                state.stack_time_points();
                state.assign_colors();
                duckdb::write(state, out.output, out.force)?;
            }
        }
        Commands::Legacy { out, .. } => {
            state.assign_colors();
            visualize::emit_interactive_visualization(&state, out.output, out.force)?;
        }
        Commands::NVTXW {
            backend,
            output,
            force,
            merge,
            ..
        } => {
            #[cfg(feature = "nvtxw")]
            {
                state.stack_time_points();
                state.assign_colors();
                let zero_time = state.zero_time;
                nvtxw::write(state, backend, output, force, merge, zero_time)?;
            }
        }
        Commands::View { .. } => {
            #[cfg(feature = "viewer")]
            {
                state.stack_time_points();
                state.assign_colors();
                viewer::start(state);
            }
        }
        Commands::Serve { host, port, .. } => {
            #[cfg(feature = "server")]
            {
                state.stack_time_points();
                state.assign_colors();
                server::start(state, &host, port);
            }
        }
        Commands::Statistics { .. } => {
            analyze::analyze_statistics(&state);
        }
        Commands::Trace { out, .. } => {
            trace_viewer::emit_trace(&state, out.output, out.force)?;
        }
        Commands::Attach { .. } | Commands::Dump { .. } => unreachable!(),
    }

    Ok(())
}
