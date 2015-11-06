#include "realm/realm.h"

#include <cstdio>
#include <cstdlib>
#include <cassert>
#include <cstring>
#include <csignal>
#include <cmath>

#include <time.h>
#include <unistd.h>

#include "philox.h"

using namespace Realm;

// Task IDs, some IDs are reserved so start at first available number
enum {
  TOP_LEVEL_TASK = Processor::TASK_ID_FIRST_AVAILABLE+0,
  INIT_CIRCUIT_DATA_TASK,
  INIT_PENNANT_DATA_TASK,
  INIT_MINIAERO_DATA_TASK,
};

// we're going to use alarm() as a watchdog to detect deadlocks
void sigalrm_handler(int sig)
{
  fprintf(stderr, "HELP!  Alarm triggered - likely deadlock!\n");
  exit(1);
}

template <typename T, T DEFVAL>
class WithDefault {
public:
  WithDefault(void) : val(DEFVAL) {}
  WithDefault(T _val) : val(_val) {}
  WithDefault<T,DEFVAL>& operator=(T _val) { val = _val; return *this; }
  operator T(void) const { return val; }
protected:
  T val;
};

Logger log_app("app");

class TestInterface {
public:
  virtual ~TestInterface(void) {}

  virtual void print_info(void) = 0;

  virtual Event initialize_data(const std::vector<Memory>& memories,
				const std::vector<Processor>& procs) = 0;

  virtual Event perform_partitioning(void) = 0;

  virtual int check_partitioning(void) = 0;
};

// generic configuration settings
namespace {
  int random_seed = 12345;
  bool random_colors = false;
  bool wait_on_events = false;
  bool show_graph = false;
  bool skip_check = false;
  TestInterface *testcfg = 0;
};

template <typename T>
void split_evenly(T total, T pieces, std::vector<T>& cuts)
{
  cuts.resize(pieces + 1);
  for(T i = 0; i <= pieces; i++)
    cuts[i] = (total * i) / pieces;
}

template <typename T>
int find_split(const std::vector<T>& cuts, T v)
{
  // dumb linear search
  assert(v >= cuts[0]);
  for(size_t i = 1; i < cuts.size(); i++)
    if(v < cuts[i])
      return i - 1;
  assert(false);
  return 0;
}

class MiniAeroTest : public TestInterface {
public:
  enum ProblemType {
    PTYPE_0,
    PTYPE_1,
    PTYPE_2,
  };
  enum FaceType {
    BC_INTERIOR = 0,
    BC_TANGENT = 1,
    BC_EXTRAPOLATE = 2,
    BC_INFLOW = 3,
    BC_NOSLIP = 4,
    BC_BLOCK_BORDER = 5,
    BC_TOTAL = 6,
  };

  WithDefault<ProblemType, PTYPE_0> problem_type;
  WithDefault<int,  4> global_x, global_y, global_z;
  WithDefault<int,   2> blocks_x, blocks_y, blocks_z;

  int n_cells;  // total cell count
  int n_blocks; // total block count
  int n_faces;  // total face count
  std::vector<int> xsplit, ysplit, zsplit;  // cut planes
  std::vector<int> cells_per_block, faces_per_block;

  MiniAeroTest(int argc, const char *argv[])
  {
#define INT_ARG(s, v) if(!strcmp(argv[i], s)) { v = atoi(argv[++i]); continue; }
    for(int i = 1; i < argc; i++) {
      INT_ARG("-type", (int&)problem_type);
      INT_ARG("-gx",   global_x);
      INT_ARG("-gy",   global_y);
      INT_ARG("-gz",   global_z);
      INT_ARG("-bx",   blocks_x);
      INT_ARG("-by",   blocks_y);
      INT_ARG("-bz",   blocks_z);
    }
#undef INT_ARG

    // don't allow degenerate blocks
    assert(global_x >= blocks_x);
    assert(global_y >= blocks_y);
    assert(global_z >= blocks_z);

    split_evenly<int>(global_x, blocks_x, xsplit);
    split_evenly<int>(global_y, blocks_y, ysplit);
    split_evenly<int>(global_z, blocks_z, zsplit);

    n_blocks = blocks_x * blocks_y * blocks_z;
    n_cells = 0;
    n_faces = 0;
    for(int bz = 0; bz < blocks_z; bz++)
      for(int by = 0; by < blocks_y; by++)
	for(int bx = 0; bx < blocks_x; bx++) {
          int nx = xsplit[bx + 1] - xsplit[bx];
          int ny = ysplit[by + 1] - ysplit[by];
          int nz = zsplit[bz + 1] - zsplit[bz];

	  int c = nx * ny * nz;
	  int f = (((nx + 1) * ny * nz) +
		   (nx * (ny + 1) * nz) +
		   (nx * ny * (nz + 1)));
	  cells_per_block.push_back(c);
	  faces_per_block.push_back(f);

	  n_cells += c;
	  n_faces += f;
        }
    assert(n_cells == global_x * global_y * global_z);
    assert(n_faces == (((global_x + blocks_x) * global_y * global_z) +
		       (global_x * (global_y + blocks_y) * global_z) +
		       (global_x * global_y * (global_z + blocks_z))));
  }

  virtual void print_info(void)
  {
    printf("Realm dependent partitioning test - miniaero: %d x %d x %d cells, %d x %d x %d blocks\n",
           (int)global_x, (int)global_y, (int)global_z,
           (int)blocks_x, (int)blocks_y, (int)blocks_z);
  }

  ZIndexSpace<1> is_cells, is_faces;
  std::vector<RegionInstance> ri_cells;
  std::vector<FieldDataDescriptor<ZIndexSpace<1>, int> > cell_blockid_field_data;
  std::vector<RegionInstance> ri_faces;
  std::vector<FieldDataDescriptor<ZIndexSpace<1>, ZPoint<1> > > face_left_field_data;
  std::vector<FieldDataDescriptor<ZIndexSpace<1>, ZPoint<1> > > face_right_field_data;
  std::vector<FieldDataDescriptor<ZIndexSpace<1>, int> > face_type_field_data;
  
  struct InitDataArgs {
    int index;
    RegionInstance ri_cells, ri_faces;
  };

  virtual Event initialize_data(const std::vector<Memory>& memories,
				const std::vector<Processor>& procs)
  {
    // top level index spaces
    is_cells = ZRect<1>(0, n_cells - 1);
    is_faces = ZRect<1>(0, n_faces - 1);

    // weighted partitions based on the distribution we already computed
    std::vector<ZIndexSpace<1> > ss_cells_w;
    std::vector<ZIndexSpace<1> > ss_faces_w;

    is_cells.create_weighted_subspaces(n_blocks, 1, cells_per_block, ss_cells_w,
				       Realm::ProfilingRequestSet()).wait();
    is_faces.create_weighted_subspaces(n_blocks, 1, faces_per_block, ss_faces_w,
				       Realm::ProfilingRequestSet()).wait();

    log_app.debug() << "Initial partitions:";
    for(size_t i = 0; i < ss_cells_w.size(); i++)
      log_app.debug() << " Cells #" << i << ": " << ss_cells_w[i];
    for(size_t i = 0; i < ss_faces_w.size(); i++)
      log_app.debug() << " Faces #" << i << ": " << ss_faces_w[i];

    // create instances for each of these subspaces
    std::vector<size_t> cell_fields, face_fields;
    cell_fields.push_back(sizeof(int));  // blockid
    assert(sizeof(int) == sizeof(ZPoint<1>));
    face_fields.push_back(sizeof(ZPoint<1>));  // left
    face_fields.push_back(sizeof(ZPoint<1>));  // right
    face_fields.push_back(sizeof(int));  // type

    ri_cells.resize(n_blocks);
    cell_blockid_field_data.resize(n_blocks);

    for(size_t i = 0; i < ss_cells_w.size(); i++) {
      RegionInstance ri = RegionInstance::create_instance(memories[i % memories.size()],
							  ss_cells_w[i],
							  cell_fields,
							  Realm::ProfilingRequestSet());
      ri_cells[i] = ri;
    
      cell_blockid_field_data[i].index_space = ss_cells_w[i];
      cell_blockid_field_data[i].inst = ri_cells[i];
      cell_blockid_field_data[i].field_offset = 0;
    }

    ri_faces.resize(n_blocks);
    face_left_field_data.resize(n_blocks);
    face_right_field_data.resize(n_blocks);
    face_type_field_data.resize(n_blocks);

    for(size_t i = 0; i < ss_faces_w.size(); i++) {
      RegionInstance ri = RegionInstance::create_instance(memories[i % memories.size()],
							  ss_faces_w[i],
							  face_fields,
							  Realm::ProfilingRequestSet());
      ri_faces[i] = ri;

      face_left_field_data[i].index_space = ss_faces_w[i];
      face_left_field_data[i].inst = ri_faces[i];
      face_left_field_data[i].field_offset = 0 * sizeof(ZPoint<1>);
      
      face_right_field_data[i].index_space = ss_faces_w[i];
      face_right_field_data[i].inst = ri_faces[i];
      face_right_field_data[i].field_offset = 1 * sizeof(ZPoint<1>);

      face_type_field_data[i].index_space = ss_faces_w[i];
      face_type_field_data[i].inst = ri_faces[i];
      face_type_field_data[i].field_offset = 2 * sizeof(ZPoint<1>);
    }

    // fire off tasks to initialize data
    std::set<Event> events;
    for(int i = 0; i < n_blocks; i++) {
      Processor p = procs[i % memories.size()];
      InitDataArgs args;
      args.index = i;
      args.ri_cells = ri_cells[i];
      args.ri_faces = ri_faces[i];
      Event e = p.spawn(INIT_MINIAERO_DATA_TASK, &args, sizeof(args));
      events.insert(e);
    }

    return Event::merge_events(events);
  }

  static void init_data_task_wrapper(const void *args, size_t arglen, Processor p)
  {
    MiniAeroTest *me = (MiniAeroTest *)testcfg;
    me->init_data_task(args, arglen, p);
  }

  ZPoint<1> global_cell_pointer(int cx, int cy, int cz)
  {
    int p = 0;

    // out of range?  return -1
    if((cx < 0) || (cx >= global_x) ||
       (cy < 0) || (cy >= global_y) ||
       (cz < 0) || (cz >= global_z))
      return -1;

    // first chunks in z, then y, then x
    int zi = find_split(zsplit, cz);
    p += global_x * global_y * zsplit[zi];
    cz -= zsplit[zi];
    int local_z = zsplit[zi + 1] - zsplit[zi];

    int yi = find_split(ysplit, cy);
    p += global_x * ysplit[yi] * local_z;
    cy -= ysplit[yi];
    int local_y = ysplit[yi + 1] - ysplit[yi];

    int xi = find_split(xsplit, cx);
    p += xsplit[xi] * local_y * local_z;
    cx -= xsplit[xi];
    int local_x = xsplit[xi + 1] - xsplit[xi];

    // now local addressing within this block
    p += (cx +
	  (cy * local_x) +
	  (cz * local_x * local_y));
    return p;
  }

  void init_data_task(const void *args, size_t arglen, Processor p)
  {
    const InitDataArgs& i_args = *(const InitDataArgs *)args;

    log_app.info() << "init task #" << i_args.index << " (ri_cells=" << i_args.ri_cells << ", ri_faces=" << i_args.ri_faces << ")";

    ZIndexSpace<1> is_cells = i_args.ri_cells.get_indexspace<1>();
    ZIndexSpace<1> is_faces = i_args.ri_faces.get_indexspace<1>();

    log_app.debug() << "C: " << is_cells;
    log_app.debug() << "F: " << is_faces;
    
    int bx = i_args.index % blocks_x;
    int by = (i_args.index / blocks_x) % blocks_y;
    int bz = i_args.index / blocks_x / blocks_y;

    int nx = xsplit[bx + 1] - xsplit[bx];
    int ny = ysplit[by + 1] - ysplit[by];
    int nz = zsplit[bz + 1] - zsplit[bz];

    size_t c = nx * ny * nz;
    size_t f = (((nx + 1) * ny * nz) +
		(nx * (ny + 1) * nz) +
		(nx * ny * (nz + 1)));
    assert(is_cells.bounds.volume() == c);
    assert(is_faces.bounds.volume() == f);

    // cells are all assigned to the local block
    {
      AffineAccessor<int,1> a_cell_blockid(i_args.ri_cells, 0 /* offset */);

      for(int cz = zsplit[bz]; cz < zsplit[bz + 1]; cz++)
	for(int cy = ysplit[by]; cy < ysplit[by + 1]; cy++)
	  for(int cx = xsplit[bx]; cx < xsplit[bx + 1]; cx++) {
	    ZPoint<1> pz = global_cell_pointer(cx, cy, cz);
	    assert(is_cells.bounds.contains(pz));

	    a_cell_blockid.write(pz, i_args.index);
	  }
    }

    // faces aren't in any globally-visible order
    {
      AffineAccessor<ZPoint<1>,1> a_face_left(i_args.ri_faces, 0 * sizeof(ZPoint<1>) /* offset */);
      AffineAccessor<ZPoint<1>,1> a_face_right(i_args.ri_faces, 1 * sizeof(ZPoint<1>) /* offset */);
      AffineAccessor<int,1> a_face_type(i_args.ri_faces, 2 * sizeof(ZPoint<1>) /* offset */);
      
      ZPoint<1> pf = is_faces.bounds.lo;

      //  --           type 0      | type 1      | type 2
      //  --           ------      | ------      | ------
      //  -- left      extrapolate | inflow      | inflow
      //  -- right     extrapolate | extrapolate | extrapolate
      //  -- down      tangent     | noslip      | tangent
      //  -- up        tangent     | extrapolate | tangent
      //  -- back      tangent     | tangent     | tangent
      //  -- front     tangent     | tangent     | tangent

      // left/right faces first
      for(int fx = xsplit[bx]; fx <= xsplit[bx + 1]; fx++) {
	int ftype = BC_INTERIOR;
	bool reversed = false;
	if(fx == xsplit[bx]) {
	  // low boundary
	  reversed = true;
	  if(fx == 0)
	    switch(problem_type) {
	    case PTYPE_0: ftype = BC_EXTRAPOLATE; break;
	    case PTYPE_1: ftype = BC_INFLOW; break;
	    case PTYPE_2: ftype = BC_INFLOW; break;
	    }
	  else
	    ftype = BC_BLOCK_BORDER;
	} else if(fx == xsplit[bx + 1]) {
	  // high boundary
	  if(fx == global_x)
	    switch(problem_type) {
	    case PTYPE_0: ftype = BC_EXTRAPOLATE; break;
	    case PTYPE_1: ftype = BC_EXTRAPOLATE; break;
	    case PTYPE_2: ftype = BC_EXTRAPOLATE; break;
	    }
	  else
	    ftype = BC_BLOCK_BORDER;
	}

	for(int cz = zsplit[bz]; cz < zsplit[bz + 1]; cz++)
	  for(int cy = ysplit[by]; cy < ysplit[by + 1]; cy++) {
	    a_face_left.write(pf, global_cell_pointer(fx - (reversed ? 0 : 1), cy, cz));
	    a_face_right.write(pf, global_cell_pointer(fx - (reversed ? 1 : 0), cy, cz));
	    a_face_type.write(pf, ftype);
	    pf.x++;
	  }
      }

      // down/up faces next
      for(int fy = ysplit[by]; fy <= ysplit[by + 1]; fy++) {
	int ftype = BC_INTERIOR;
	bool reversed = false;
	if(fy == ysplit[by]) {
	  // low boundary
	  reversed = true;
	  if(fy == 0)
	    switch(problem_type) {
	    case PTYPE_0: ftype = BC_TANGENT; break;
	    case PTYPE_1: ftype = BC_NOSLIP; break;
	    case PTYPE_2: ftype = BC_TANGENT; break;
	    }
	  else
	    ftype = BC_BLOCK_BORDER;
	} else if(fy == ysplit[by + 1]) {
	  // high boundary
	  if(fy == global_y)
	    switch(problem_type) {
	    case PTYPE_0: ftype = BC_TANGENT; break;
	    case PTYPE_1: ftype = BC_EXTRAPOLATE; break;
	    case PTYPE_2: ftype = BC_TANGENT; break;
	    }
	  else
	    ftype = BC_BLOCK_BORDER;
	}

	for(int cz = zsplit[bz]; cz < zsplit[bz + 1]; cz++)
	  for(int cx = xsplit[bx]; cx < xsplit[bx + 1]; cx++) {
	    a_face_left.write(pf, global_cell_pointer(cx, fy - (reversed ? 0 : 1), cz));
	    a_face_right.write(pf, global_cell_pointer(cx, fy - (reversed ? 1 : 0), cz));
	    a_face_type.write(pf, ftype);
	    pf.x++;
	  }
      }

      // back/front faces last
      for(int fz = zsplit[bz]; fz <= zsplit[bz + 1]; fz++) {
	int ftype = BC_INTERIOR;
	bool reversed = false;
	if(fz == zsplit[bz]) {
	  // low boundary
	  reversed = true;
	  if(fz == 0)
	    switch(problem_type) {
	    case PTYPE_0: ftype = BC_TANGENT; break;
	    case PTYPE_1: ftype = BC_TANGENT; break;
	    case PTYPE_2: ftype = BC_TANGENT; break;
	    }
	  else
	    ftype = BC_BLOCK_BORDER;
	} else if(fz == zsplit[bz + 1]) {
	  // high boundary
	  if(fz == global_z)
	    switch(problem_type) {
	    case PTYPE_0: ftype = BC_TANGENT; break;
	    case PTYPE_1: ftype = BC_TANGENT; break;
	    case PTYPE_2: ftype = BC_TANGENT; break;
	    }
	  else
	    ftype = BC_BLOCK_BORDER;
	}

	for(int cy = ysplit[by]; cy < ysplit[by + 1]; cy++)
	  for(int cx = xsplit[bx]; cx < xsplit[bx + 1]; cx++) {
	    a_face_left.write(pf, global_cell_pointer(cx, cy, fz - (reversed ? 0 : 1)));
	    a_face_right.write(pf, global_cell_pointer(cx, cy, fz - (reversed ? 1 : 0)));
	    a_face_type.write(pf, ftype);
	    pf.x++;
	  }
      }

      assert(pf.x == is_faces.bounds.hi.x + 1);
    }
    
    if(show_graph) {
      AffineAccessor<int,1> a_cell_blockid(i_args.ri_cells, 0 /* offset */);

      for(int i = is_cells.bounds.lo; i <= is_cells.bounds.hi; i++)
	std::cout << "Z[" << i << "]: blockid=" << a_cell_blockid.read(i) << std::endl;

      AffineAccessor<ZPoint<1>,1> a_face_left(i_args.ri_faces, 0 * sizeof(ZPoint<1>) /* offset */);
      AffineAccessor<ZPoint<1>,1> a_face_right(i_args.ri_faces, 1 * sizeof(ZPoint<1>) /* offset */);
      AffineAccessor<int,1> a_face_type(i_args.ri_faces, 2 * sizeof(ZPoint<1>) /* offset */);

      for(int i = is_faces.bounds.lo; i <= is_faces.bounds.hi; i++)
	std::cout << "S[" << i << "]:"
		  << " left=" << a_face_left.read(i)
		  << " right=" << a_face_right.read(i)
		  << " type=" << a_face_type.read(i)
		  << std::endl;
    }
  }

  // the outputs of our partitioning will be:
  //  p_cells               - subsets of is_cells split by block
  //  p_faces               - subsets of_is_faces split by block (based on left cell)
  //  p_facetypes[6]        - subsets of p_faces split further by face type
  //  p_ghost               - subsets of is_cells reachable by each block's boundary faces

  std::vector<ZIndexSpace<1> > p_cells;
  std::vector<ZIndexSpace<1> > p_faces;
  std::vector<std::vector<ZIndexSpace<1> > > p_facetypes;
  std::vector<ZIndexSpace<1> > p_ghost;

  virtual Event perform_partitioning(void)
  {
    // partition cells first
    std::vector<int> colors(n_blocks);
    for(int i = 0; i < n_blocks; i++)
      colors[i] = i;

    Event e1 = is_cells.create_subspaces_by_field(cell_blockid_field_data,
						  colors,
						  p_cells,
						  Realm::ProfilingRequestSet());
    if(wait_on_events) e1.wait();

    // now a preimage to get faces
    Event e2 = is_faces.create_subspaces_by_preimage(face_left_field_data,
						     p_cells,
						     p_faces,
						     Realm::ProfilingRequestSet(),
						     e1);
    if(wait_on_events) e2.wait();

    // now split by face type
    std::set<Event> evs;
    std::vector<int> ftcolors(BC_TOTAL);
    for(int i = 0; i < BC_TOTAL; i++)
      ftcolors[i] = i;
    p_facetypes.resize(n_blocks);
    std::vector<ZIndexSpace<1> > p_border_faces(n_blocks);
    
    for(int idx = 0; idx < n_blocks; idx++) {
      Event e = p_faces[idx].create_subspaces_by_field(face_type_field_data,
						       ftcolors,
						       p_facetypes[idx],
						       Realm::ProfilingRequestSet(),
						       e2);
      if(wait_on_events) e.wait();
      evs.insert(e);
      p_border_faces[idx] = p_facetypes[idx][BC_BLOCK_BORDER];
    }
    Event e3 = Event::merge_events(evs);

    // finally, the image of just the boundary faces through the right face gets us
    //  ghost cells
    Event e4 = is_cells.create_subspaces_by_image(face_right_field_data,
						  p_border_faces,
						  p_ghost,
						  Realm::ProfilingRequestSet(),
						  e3);
    if(wait_on_events) e4.wait();

    return e4;
  }

  virtual int check_partitioning(void)
  {
    int errors = 0;

    ZPoint<1> pc = is_cells.bounds.lo;
    ZPoint<1> pf = is_faces.bounds.lo;

    for(int blkid = 0; blkid < n_blocks; blkid++) {
      int bx = blkid % blocks_x;
      int by = (blkid / blocks_x) % blocks_y;
      int bz = blkid / blocks_x / blocks_y;

      int nx = xsplit[bx + 1] - xsplit[bx];
      int ny = ysplit[by + 1] - ysplit[by];
      int nz = zsplit[bz + 1] - zsplit[bz];

      // check cells
      for(int i = 0; i < cells_per_block[blkid]; i++) {
	for(int j = 0; j < n_blocks; j++) {
	  bool exp = (j == blkid);
	  bool act = p_cells[j].contains(pc);
	  if(exp != act) {
	    log_app.error() << "mismatch: cell " << pc << " in p_cells[" << j << "]: exp=" << exp << " act=" << act;
	    errors++;
	  }
	}

	std::set<int> exp_ghosts;
	int cx = i % nx;
	int cy = (i / nx) % ny;
	int cz = i / nx / ny;
	if((cx == 0) && (bx > 0))
	  exp_ghosts.insert(blkid - 1);
	if((cx == (nx - 1)) && (bx < (blocks_x - 1)))
	  exp_ghosts.insert(blkid + 1);
	if((cy == 0) && (by > 0))
	  exp_ghosts.insert(blkid - blocks_x);
	if((cy == (ny - 1)) && (by < (blocks_y - 1)))
	  exp_ghosts.insert(blkid + blocks_x);
	if((cz == 0) && (bz > 0))
	  exp_ghosts.insert(blkid - blocks_x * blocks_y);
	if((cz == (nz - 1)) && (bz < (blocks_z - 1)))
	  exp_ghosts.insert(blkid + blocks_x * blocks_y);

	for(int j = 0; j < n_blocks; j++) {
	  bool exp = exp_ghosts.count(j) > 0;
	  bool act = p_ghost[j].contains(pc);
	  if(exp != act) {
	    log_app.error() << "mismatch: cell " << pc << " in p_ghost[" << j << "]: exp=" << exp << " act=" << act;
	    errors++;
	  }
	}

	pc.x++;
      }

      // check faces
      for(int i = 0; i < faces_per_block[blkid]; i++) {
	for(int j = 0; j < n_blocks; j++) {
	  bool exp = (j == blkid);
	  bool act = p_faces[j].contains(pf);
	  if(exp != act) {
	    log_app.error() << "mismatch: face " << pf << " in p_faces[" << j << "]: exp=" << exp << " act=" << act;
	    errors++;
	  }
	  FaceType exptype = BC_INTERIOR;
	  // luckily the faces on the edge of a block come in chunks
	  int lr_faces = (nx + 1) * ny * nz;
	  int du_faces = nx * (ny + 1) * nz;
	  int bf_faces = nx * ny * (nz + 1);
	  assert((lr_faces + du_faces + bf_faces) == faces_per_block[blkid]);
	  if(i < lr_faces) {
	    int x = i / ny / nz;
	    if(x == 0)
	      exptype = ((bx == 0) ?
			 ((problem_type == PTYPE_0) ? BC_EXTRAPOLATE :
			  (problem_type == PTYPE_1) ? BC_INFLOW :
			                              BC_INFLOW) :
			 BC_BLOCK_BORDER);
	    if(x == nx)
	      exptype = ((bx == blocks_x - 1) ?
			 ((problem_type == PTYPE_0) ? BC_EXTRAPOLATE :
			  (problem_type == PTYPE_1) ? BC_EXTRAPOLATE :
			                              BC_EXTRAPOLATE) :
			 BC_BLOCK_BORDER);
	  } else if(i < (lr_faces + du_faces)) {
	    int y = (i - lr_faces) / nx / nz;
	    if(y == 0)
	      exptype = ((by == 0) ?
			 ((problem_type == PTYPE_0) ? BC_TANGENT :
			  (problem_type == PTYPE_1) ? BC_NOSLIP :
			                              BC_TANGENT) :
			 BC_BLOCK_BORDER);
	    if(y == ny)
	      exptype = ((by == blocks_y - 1) ?
			 ((problem_type == PTYPE_0) ? BC_TANGENT :
			  (problem_type == PTYPE_1) ? BC_EXTRAPOLATE :
			                              BC_TANGENT) :
			 BC_BLOCK_BORDER);
	  } else {
	    int z = (i - lr_faces - du_faces) / nx / ny;
	    if(z == 0)
	      exptype = ((bz == 0) ?
			 ((problem_type == PTYPE_0) ? BC_TANGENT :
			  (problem_type == PTYPE_1) ? BC_TANGENT :
			                              BC_TANGENT) :
			 BC_BLOCK_BORDER);
	    if(z == nz)
	      exptype = ((bz == blocks_z - 1) ?
			 ((problem_type == PTYPE_0) ? BC_TANGENT :
			  (problem_type == PTYPE_1) ? BC_TANGENT :
			                              BC_TANGENT) :
			 BC_BLOCK_BORDER);
	  }
	  
	  for(int k = 0; k < BC_TOTAL; k++) {
	    bool exp = (j == blkid) && (k == exptype);
	    bool act = p_facetypes[j][k].contains(pf);
	    if(exp != act) {
	      log_app.error() << "mismatch: face " << pf << " in p_facetypes[" << j << "][" << k << "]: exp=" << exp << " act=" << act;
	      errors++;
	    }
	  }
	}
	pf.x++;
      }
    }

    return errors;
  }
};

class CircuitTest : public TestInterface {
public:
  // graph config parameters
  WithDefault<int, 100> num_nodes;
  WithDefault<int,  10> num_edges;
  WithDefault<int,   2> num_pieces;
  WithDefault<int,  50> pct_wire_in_piece;

  CircuitTest(int argc, const char *argv[])
  {
    for(int i = 1; i < argc; i++) {
      if(!strcmp(argv[i], "-n")) {
	num_nodes = atoi(argv[++i]);
	continue;
      }

      if(!strcmp(argv[i], "-e")) {
	num_edges = atoi(argv[++i]);
	continue;
      }

      if(!strcmp(argv[i], "-p")) {
	num_pieces = atoi(argv[++i]);
	continue;
      }
    }
  }

  struct InitDataArgs {
    int index;
    RegionInstance ri_nodes, ri_edges;
  };

  enum PRNGStreams {
    NODE_SUBCKT_STREAM,
    EDGE_IN_NODE_STREAM,
    EDGE_OUT_NODE_STREAM1,
    EDGE_OUT_NODE_STREAM2,
  };

  // nodes and edges are generated pseudo-randomly so that we can check the results without
  //  needing all the field data in any one place
  void random_node_data(int idx, int& subckt)
  {
    if(random_colors)
      subckt = Philox_2x32<>::rand_int(random_seed, idx, NODE_SUBCKT_STREAM, num_pieces);
    else
      subckt = idx * num_pieces / num_nodes;
  }

  void random_edge_data(int idx, ZPoint<1>& in_node, ZPoint<1>& out_node)
  {
    if(random_colors) {
      in_node = Philox_2x32<>::rand_int(random_seed, idx, EDGE_IN_NODE_STREAM, num_nodes);
      out_node = Philox_2x32<>::rand_int(random_seed, idx, EDGE_OUT_NODE_STREAM1, num_nodes);
    } else {
      int subckt = idx * num_pieces / num_edges;
      int n_lo = subckt * num_nodes / num_pieces;
      int n_hi = (subckt + 1) * num_nodes / num_pieces;
      in_node = n_lo + Philox_2x32<>::rand_int(random_seed, idx, EDGE_IN_NODE_STREAM,
					      n_hi - n_lo);
      int pct = Philox_2x32<>::rand_int(random_seed, idx, EDGE_OUT_NODE_STREAM2, 100);
      if(pct < pct_wire_in_piece)
	out_node = n_lo + Philox_2x32<>::rand_int(random_seed, idx, EDGE_OUT_NODE_STREAM1,
						 n_hi - n_lo);
      else
	out_node = Philox_2x32<>::rand_int(random_seed, idx, EDGE_OUT_NODE_STREAM1, num_nodes);	
    }
  }

  static void init_data_task_wrapper(const void *args, size_t arglen, Processor p)
  {
    CircuitTest *me = (CircuitTest *)testcfg;
    me->init_data_task(args, arglen, p);
  }

  void init_data_task(const void *args, size_t arglen, Processor p)
  {
    const InitDataArgs& i_args = *(const InitDataArgs *)args;

    log_app.info() << "init task #" << i_args.index << " (ri_nodes=" << i_args.ri_nodes << ", ri_edges=" << i_args.ri_edges << ")";

    ZIndexSpace<1> is_nodes = i_args.ri_nodes.get_indexspace<1>();
    ZIndexSpace<1> is_edges = i_args.ri_edges.get_indexspace<1>();

    log_app.debug() << "N: " << is_nodes;
    log_app.debug() << "E: " << is_edges;
    
    {
      AffineAccessor<int,1> a_subckt_id(i_args.ri_nodes, 0 /* offset */);
      
      for(int i = is_nodes.bounds.lo; i <= is_nodes.bounds.hi; i++) {
	int subckt;
	random_node_data(i, subckt);
	a_subckt_id.write(i, subckt);
      }
    }
    
    {
      AffineAccessor<ZPoint<1>,1> a_in_node(i_args.ri_edges, 0 * sizeof(ZPoint<1>) /* offset */);
      AffineAccessor<ZPoint<1>,1> a_out_node(i_args.ri_edges, 1 * sizeof(ZPoint<1>) /* offset */);
      
      for(int i = is_edges.bounds.lo; i <= is_edges.bounds.hi; i++) {
	ZPoint<1> in_node, out_node;
	random_edge_data(i, in_node, out_node);
	a_in_node.write(i, in_node);
	a_out_node.write(i, out_node);
      }
    }

    if(show_graph) {
      AffineAccessor<int,1> a_subckt_id(i_args.ri_nodes, 0 /* offset */);

      for(int i = is_nodes.bounds.lo; i <= is_nodes.bounds.hi; i++)
	std::cout << "subckt_id[" << i << "] = " << a_subckt_id.read(i) << std::endl;

      AffineAccessor<ZPoint<1>,1> a_in_node(i_args.ri_edges, 0 * sizeof(ZPoint<1>) /* offset */);

      for(int i = is_edges.bounds.lo; i <= is_edges.bounds.hi; i++)
	std::cout << "in_node[" << i << "] = " << a_in_node.read(i) << std::endl;

      AffineAccessor<ZPoint<1>,1> a_out_node(i_args.ri_edges, 1 * sizeof(ZPoint<1>) /* offset */);

      for(int i = is_edges.bounds.lo; i <= is_edges.bounds.hi; i++)
	std::cout << "out_node[" << i << "] = " << a_out_node.read(i) << std::endl;
    }
  }

  ZIndexSpace<1> is_nodes, is_edges;
  std::vector<RegionInstance> ri_nodes;
  std::vector<FieldDataDescriptor<ZIndexSpace<1>, int> > subckt_field_data;
  std::vector<RegionInstance> ri_edges;
  std::vector<FieldDataDescriptor<ZIndexSpace<1>, ZPoint<1> > > in_node_field_data;
  std::vector<FieldDataDescriptor<ZIndexSpace<1>, ZPoint<1> > > out_node_field_data;

  virtual void print_info(void)
  {
    printf("Realm dependent partitioning test - circuit: %d nodes, %d edges, %d pieces\n",
	   (int)num_nodes, (int)num_edges, (int)num_pieces);
  }

  virtual Event initialize_data(const std::vector<Memory>& memories,
				const std::vector<Processor>& procs)
  {
    // now create index spaces for nodes and edges
    is_nodes = ZRect<1>(0, num_nodes - 1);
    is_edges = ZRect<1>(0, num_edges - 1);

    // equal partition is used to do initial population of edges and nodes
    std::vector<ZIndexSpace<1> > ss_nodes_eq;
    std::vector<ZIndexSpace<1> > ss_edges_eq;

    is_nodes.create_equal_subspaces(num_pieces, 1, ss_nodes_eq, Realm::ProfilingRequestSet()).wait();
    is_edges.create_equal_subspaces(num_pieces, 1, ss_edges_eq, Realm::ProfilingRequestSet()).wait();

    log_app.debug() << "Initial partitions:";
    for(size_t i = 0; i < ss_nodes_eq.size(); i++)
      log_app.debug() << " Nodes #" << i << ": " << ss_nodes_eq[i];
    for(size_t i = 0; i < ss_edges_eq.size(); i++)
      log_app.debug() << " Edges #" << i << ": " << ss_edges_eq[i];

    // create instances for each of these subspaces
    std::vector<size_t> node_fields, edge_fields;
    node_fields.push_back(sizeof(int));  // subckt_id
    assert(sizeof(int) == sizeof(ZPoint<1>));
    edge_fields.push_back(sizeof(ZPoint<1>));  // in_node
    edge_fields.push_back(sizeof(ZPoint<1>));  // out_node

    ri_nodes.resize(num_pieces);
    subckt_field_data.resize(num_pieces);

    for(size_t i = 0; i < ss_nodes_eq.size(); i++) {
      RegionInstance ri = RegionInstance::create_instance(memories[i % memories.size()],
							  ss_nodes_eq[i],
							  node_fields,
							  Realm::ProfilingRequestSet());
      ri_nodes[i] = ri;
    
      subckt_field_data[i].index_space = ss_nodes_eq[i];
      subckt_field_data[i].inst = ri_nodes[i];
      subckt_field_data[i].field_offset = 0;
    }

    ri_edges.resize(num_pieces);
    in_node_field_data.resize(num_pieces);
    out_node_field_data.resize(num_pieces);

    for(size_t i = 0; i < ss_edges_eq.size(); i++) {
      RegionInstance ri = RegionInstance::create_instance(memories[i % memories.size()],
							  ss_edges_eq[i],
							  edge_fields,
							  Realm::ProfilingRequestSet());
      ri_edges[i] = ri;

      in_node_field_data[i].index_space = ss_edges_eq[i];
      in_node_field_data[i].inst = ri_edges[i];
      in_node_field_data[i].field_offset = 0 * sizeof(ZPoint<1>);
      
      out_node_field_data[i].index_space = ss_edges_eq[i];
      out_node_field_data[i].inst = ri_edges[i];
      out_node_field_data[i].field_offset = 1 * sizeof(ZPoint<1>);
    }

    // fire off tasks to initialize data
    std::set<Event> events;
    for(int i = 0; i < num_pieces; i++) {
      Processor p = procs[i % memories.size()];
      InitDataArgs args;
      args.index = i;
      args.ri_nodes = ri_nodes[i];
      args.ri_edges = ri_edges[i];
      Event e = p.spawn(INIT_CIRCUIT_DATA_TASK, &args, sizeof(args));
      events.insert(e);
    }

    return Event::merge_events(events);
  }

  // the outputs of our partitioning will be:
  //  is_private, is_shared - subsets of is_nodes based on private/shared
  //  p_pvt, p_shr, p_ghost - subsets of the above split by subckt
  //  p_edges               - subsets of is_edges for each subckt

  ZIndexSpace<1> is_shared, is_private;
  std::vector<ZIndexSpace<1> > p_pvt, p_shr, p_ghost;
  std::vector<ZIndexSpace<1> > p_edges;

  virtual Event perform_partitioning(void)
  {
    // first partition nodes by subckt id (this is the independent partition,
    //  but not actually used by the app)
    std::vector<ZIndexSpace<1> > p_nodes;

    std::vector<int> colors(num_pieces);
    for(int i = 0; i < num_pieces; i++)
      colors[i] = i;

    Event e1 = is_nodes.create_subspaces_by_field(subckt_field_data,
						  colors,
						  p_nodes,
						  Realm::ProfilingRequestSet());
    if(wait_on_events) e1.wait();

    // now compute p_edges based on the color of their in_node (i.e. a preimage)
    Event e2 = is_edges.create_subspaces_by_preimage(in_node_field_data,
						     p_nodes,
						     p_edges,
						     Realm::ProfilingRequestSet(),
						     e1);
    if(wait_on_events) e2.wait();

    // an image of p_edges through out_node gives us all the shared nodes, along
    //  with some private nodes
    std::vector<ZIndexSpace<1> > p_extra_nodes;

    Event e3 = is_nodes.create_subspaces_by_image(out_node_field_data,
						  p_edges,
						  p_extra_nodes,
						  Realm::ProfilingRequestSet(),
						  e2);
    if(wait_on_events) e3.wait();
  
    // subtracting out those private nodes gives us p_ghost
    Event e4 = ZIndexSpace<1>::compute_differences(p_extra_nodes,
						   p_nodes,
						   p_ghost,
						   Realm::ProfilingRequestSet(),
						   e3);
    if(wait_on_events) e4.wait();

    // the union of everybody's ghost nodes is is_shared
    Event e5 = ZIndexSpace<1>::compute_union(p_ghost, is_shared,
					     Realm::ProfilingRequestSet(),
					     e4);
    if(wait_on_events) e5.wait();

    // and is_private is just the nodes of is_nodes that aren't in is_shared
    Event e6 = ZIndexSpace<1>::compute_difference(is_nodes, is_shared, is_private,
						  Realm::ProfilingRequestSet(),
						  e5);
    if(wait_on_events) e6.wait();

    // the intersection of the original p_nodes with is_shared gives us p_shr
    // (note that we can do this in parallel with the computation of is_private)
    Event e7 = ZIndexSpace<1>::compute_intersections(p_nodes, is_shared, p_shr,
						     Realm::ProfilingRequestSet(),
						     e5);
    if(wait_on_events) e7.wait();

    // and finally, the intersection of p_nodes with is_private gives us p_pvt
    Event e8 = ZIndexSpace<1>::compute_intersections(p_nodes, is_private, p_pvt,
						     Realm::ProfilingRequestSet(),
						     e6);
    if(wait_on_events) e8.wait();

    // all done - wait on e7 and e8, which dominate every other operation
    return Event::merge_events(e7, e8);
  }

  virtual int check_partitioning(void)
  {
    int errors = 0;

    // we'll make up the list of nodes we expect to be shared as we walk the edges
    std::map<int, std::set<int> > ghost_nodes;

    for(int i = 0; i < num_edges; i++) {
      // regenerate the random info for this edge and the two nodes it touches
      ZPoint<1> in_node, out_node;
      int in_subckt, out_subckt;
      random_edge_data(i, in_node, out_node);
      random_node_data(in_node, in_subckt);
      random_node_data(out_node, out_subckt);

      // the edge should be in exactly the p_edges for in_subckt
      for(int p = 0; p < num_pieces; p++) {
	bool exp = (p == in_subckt);
	bool act = p_edges[p].contains(i);
	if(exp != act) {
	  log_app.error() << "mismatch: edge " << i << " in p_edges[" << p << "]: exp=" << exp << " act=" << act;
	  errors++;
	}
      }

      // is the output node a ghost for this wire?
      if(in_subckt != out_subckt)
	ghost_nodes[out_node].insert(in_subckt);
    }

    // now we can check the nodes
    for(int i = 0; i < num_nodes; i++) {
      int subckt;
      random_node_data(i, subckt);
      // check is_private and is_shared first
      {
	bool exp = ghost_nodes.count(i) == 0;
	bool act = is_private.contains(i);
	if(exp != act) {
	  log_app.error() << "mismatch: node " << i << " in is_private: exp=" << exp << " act=" << act;
	  errors++;
	}
      }
      {
	bool exp = ghost_nodes.count(i) > 0;
	bool act = is_shared.contains(i);
	if(exp != act) {
	  log_app.error() << "mismatch: node " << i << " in is_shared: exp=" << exp << " act=" << act;
	  errors++;
	}
      }

      // now check p_pvt/shr/ghost
      for(int p = 0; p < num_pieces; p++) {
	bool exp = (subckt == p) && (ghost_nodes.count(i) == 0);
	bool act = p_pvt[p].contains(i);
	if(exp != act) {
	  log_app.error() << "mismatch: node " << i << " in p_pvt[" << p << "]: exp=" << exp << " act=" << act;
	  errors++;
	}
      }
      for(int p = 0; p < num_pieces; p++) {
	bool exp = (subckt == p) && (ghost_nodes.count(i) > 0);
	bool act = p_shr[p].contains(i);
	if(exp != act) {
	  log_app.error() << "mismatch: node " << i << " in p_shr[" << p << "]: exp=" << exp << " act=" << act;
	  errors++;
	}
      }
      for(int p = 0; p < num_pieces; p++) {
	bool exp = (subckt != p) && (ghost_nodes.count(i) > 0) && (ghost_nodes[i].count(p) > 0);
	bool act = p_ghost[p].contains(i);
	if(exp != act) {
	  log_app.error() << "mismatch: node " << i << " in p_ghost[" << p << "]: exp=" << exp << " act=" << act;
	  errors++;
	}
      }
    }

    return errors;
  }
};

class PennantTest : public TestInterface {
public:
public:
  // graph config parameters
  enum MeshType {
    RectangularMesh,
  };
  WithDefault<MeshType, RectangularMesh> mesh_type;
  WithDefault<int,  10> nzx;  // number of zones in x
  WithDefault<int,  10> nzy;  // number of zones in y
  WithDefault<int,   2> numpcx;  // number of submeshes in x
  WithDefault<int,   2> numpcy;  // number of submeshes in y

  int npx, npy;      // number of points in each dimension
  int nz, ns, np, numpc;  // total number of zones, sides, points, and pieces
  std::vector<int> zxbound, zybound; // x and y split points between submeshes
  std::vector<int> lz, ls, lp;  // number of zones, sides, and points in each submesh

  PennantTest(int argc, const char *argv[])
  {
#define INT_ARG(s, v) if(!strcmp(argv[i], s)) { v = atoi(argv[++i]); continue; }
    for(int i = 1; i < argc; i++) {
      INT_ARG("-nzx",    nzx)
      INT_ARG("-nzy",    nzy)
      INT_ARG("-numpcx", numpcx)
      INT_ARG("-numpcy", numpcy)
    }
#undef INT_ARG

    switch(mesh_type) {
    case RectangularMesh:
      {
	npx = nzx + 1;
	npy = nzy + 1;
	numpc = numpcx * numpcy;

	zxbound.resize(numpcx + 1);
	for(int i = 0; i <= numpcx; i++)
	  zxbound[i] = (i * nzx) / numpcx;

	zybound.resize(numpcy + 1);
	for(int i = 0; i <= numpcy; i++)
	  zybound[i] = (i * nzy) / numpcy;

	nz = ns = np = 0;
	for(int pcy = 0; pcy < numpcy; pcy++) {
	  for(int pcx = 0; pcx < numpcx; pcx++) {
	    int lx = zxbound[pcx + 1] - zxbound[pcx];
	    int ly = zybound[pcy + 1] - zybound[pcy];

	    int zones = lx * ly;
	    int sides = zones * 4;
	    // points are a little funny - shared edges go to the lower numbered piece
	    int points = ((pcx == 0) ? (lx + 1) : lx) * ((pcy == 0) ? (ly + 1) : ly);

	    lz.push_back(zones);
	    ls.push_back(sides);
	    lp.push_back(points);
	    nz += zones;
	    ns += sides;
	    np += points;
	  }
	}

	assert(nz == (nzx * nzy));
	assert(ns == (4 * nzx * nzy));
	assert(np == (npx * npy));

	break;
      }
    }
  }

  virtual void print_info(void)
  {
    printf("Realm dependent partitioning test - pennant: %d x %d zones, %d x %d pieces\n",
	   (int)nzx, (int)nzy, (int)numpcx, (int)numpcy);
  }

  ZIndexSpace<1> is_zones, is_sides, is_points;
  std::vector<RegionInstance> ri_zones;
  std::vector<FieldDataDescriptor<ZIndexSpace<1>, int> > zone_color_field_data;
  std::vector<RegionInstance> ri_sides;
  std::vector<FieldDataDescriptor<ZIndexSpace<1>, ZPoint<1> > > side_mapsz_field_data;
  std::vector<FieldDataDescriptor<ZIndexSpace<1>, ZPoint<1> > > side_mapss3_field_data;
  std::vector<FieldDataDescriptor<ZIndexSpace<1>, ZPoint<1> > > side_mapsp1_field_data;
  std::vector<FieldDataDescriptor<ZIndexSpace<1>, bool > > side_ok_field_data;

  struct InitDataArgs {
    int index;
    RegionInstance ri_zones, ri_sides;
  };

  virtual Event initialize_data(const std::vector<Memory>& memories,
				const std::vector<Processor>& procs)
  {
    // top level index spaces
    is_zones = ZRect<1>(0, nz - 1);
    is_sides = ZRect<1>(0, ns - 1);
    is_points = ZRect<1>(0, np - 1);

    // weighted partitions based on the distribution we already computed
    std::vector<ZIndexSpace<1> > ss_zones_w;
    std::vector<ZIndexSpace<1> > ss_sides_w;
    std::vector<ZIndexSpace<1> > ss_points_w;

    is_zones.create_weighted_subspaces(numpc, 1, lz, ss_zones_w, Realm::ProfilingRequestSet()).wait();
    is_sides.create_weighted_subspaces(numpc, 1, ls, ss_sides_w, Realm::ProfilingRequestSet()).wait();
    is_points.create_weighted_subspaces(numpc, 1, lp, ss_points_w, Realm::ProfilingRequestSet()).wait();

    log_app.debug() << "Initial partitions:";
    for(size_t i = 0; i < ss_zones_w.size(); i++)
      log_app.debug() << " Zones #" << i << ": " << ss_zones_w[i];
    for(size_t i = 0; i < ss_sides_w.size(); i++)
      log_app.debug() << " Sides #" << i << ": " << ss_sides_w[i];
    for(size_t i = 0; i < ss_points_w.size(); i++)
      log_app.debug() << " Points #" << i << ": " << ss_points_w[i];

    // create instances for each of these subspaces
    std::vector<size_t> zone_fields, side_fields;
    zone_fields.push_back(sizeof(int));  // color
    assert(sizeof(int) == sizeof(ZPoint<1>));
    side_fields.push_back(sizeof(ZPoint<1>));  // mapsz
    side_fields.push_back(sizeof(ZPoint<1>));  // mapss3
    side_fields.push_back(sizeof(ZPoint<1>));  // mapsp1
    side_fields.push_back(sizeof(bool));  // ok

    ri_zones.resize(numpc);
    zone_color_field_data.resize(numpc);

    for(size_t i = 0; i < ss_zones_w.size(); i++) {
      RegionInstance ri = RegionInstance::create_instance(memories[i % memories.size()],
							  ss_zones_w[i],
							  zone_fields,
							  Realm::ProfilingRequestSet());
      ri_zones[i] = ri;
    
      zone_color_field_data[i].index_space = ss_zones_w[i];
      zone_color_field_data[i].inst = ri_zones[i];
      zone_color_field_data[i].field_offset = 0;
    }

    ri_sides.resize(numpc);
    side_mapsz_field_data.resize(numpc);
    side_mapss3_field_data.resize(numpc);
    side_mapsp1_field_data.resize(numpc);
    side_ok_field_data.resize(numpc);

    for(size_t i = 0; i < ss_sides_w.size(); i++) {
      RegionInstance ri = RegionInstance::create_instance(memories[i % memories.size()],
							  ss_sides_w[i],
							  side_fields,
							  Realm::ProfilingRequestSet());
      ri_sides[i] = ri;

      side_mapsz_field_data[i].index_space = ss_sides_w[i];
      side_mapsz_field_data[i].inst = ri_sides[i];
      side_mapsz_field_data[i].field_offset = 0 * sizeof(ZPoint<1>);
      
      side_mapss3_field_data[i].index_space = ss_sides_w[i];
      side_mapss3_field_data[i].inst = ri_sides[i];
      side_mapss3_field_data[i].field_offset = 1 * sizeof(ZPoint<1>);

      side_mapsp1_field_data[i].index_space = ss_sides_w[i];
      side_mapsp1_field_data[i].inst = ri_sides[i];
      side_mapsp1_field_data[i].field_offset = 2 * sizeof(ZPoint<1>);
      
      side_ok_field_data[i].index_space = ss_sides_w[i];
      side_ok_field_data[i].inst = ri_sides[i];
      side_ok_field_data[i].field_offset = 3 * sizeof(ZPoint<1>);
    }

    // fire off tasks to initialize data
    std::set<Event> events;
    for(int i = 0; i < numpc; i++) {
      Processor p = procs[i % memories.size()];
      InitDataArgs args;
      args.index = i;
      args.ri_zones = ri_zones[i];
      args.ri_sides = ri_sides[i];
      Event e = p.spawn(INIT_PENNANT_DATA_TASK, &args, sizeof(args));
      events.insert(e);
    }

    return Event::merge_events(events);
  }

  static void init_data_task_wrapper(const void *args, size_t arglen, Processor p)
  {
    PennantTest *me = (PennantTest *)testcfg;
    me->init_data_task(args, arglen, p);
  }

  ZPoint<1> global_point_pointer(int py, int px) const
  {
    int pp = 0;

    // start by steping over whole y slabs - again be careful that the extra slab belongs to pcy == 0
    int dy;
    if(py > zybound[1]) {
      int pcy = 1;
      while(py > zybound[pcy + 1]) pcy++;
      int slabs = zybound[pcy] + 1;
      pp += npx * slabs;
      py -= slabs;
      dy = zybound[pcy + 1] - zybound[pcy];
    } else {
      dy = zybound[1] + 1;
    }

    // now chunks in x, using just the y width of this row of chunks
    int dx;
    if(px > zxbound[1]) {
      int pcx = 1;
      while(px > zxbound[pcx + 1]) pcx++;
      int strips = zxbound[pcx] + 1;
      pp += dy * strips;
      px -= strips;
      dx = zxbound[pcx + 1] - zxbound[pcx];
    } else {
      dx = zxbound[1] + 1;
    }

    // finally, px and py are now local and are handled easily
    pp += py * dx + px;

    return pp;
  }

  void init_data_task(const void *args, size_t arglen, Processor p)
  {
    const InitDataArgs& i_args = *(const InitDataArgs *)args;

    log_app.info() << "init task #" << i_args.index << " (ri_zones=" << i_args.ri_zones << ", ri_sides=" << i_args.ri_sides << ")";

    ZIndexSpace<1> is_zones = i_args.ri_zones.get_indexspace<1>();
    ZIndexSpace<1> is_sides = i_args.ri_sides.get_indexspace<1>();

    log_app.debug() << "Z: " << is_zones;
    log_app.debug() << "S: " << is_sides;
    
    int pcx = i_args.index % numpcx;
    int pcy = i_args.index / numpcx;

    int zxlo = zxbound[pcx];
    int zxhi = zxbound[pcx + 1];
    int zylo = zybound[pcy];
    int zyhi = zybound[pcy + 1];

    {
      AffineAccessor<int,1> a_zone_color(i_args.ri_zones, 0 /* offset */);
      AffineAccessor<ZPoint<1>,1> a_side_mapsz(i_args.ri_sides, 0 * sizeof(ZPoint<1>) /* offset */);
      AffineAccessor<ZPoint<1>,1> a_side_mapss3(i_args.ri_sides, 1 * sizeof(ZPoint<1>) /* offset */);
      AffineAccessor<ZPoint<1>,1> a_side_mapsp1(i_args.ri_sides, 2 * sizeof(ZPoint<1>) /* offset */);
      AffineAccessor<bool,1> a_side_ok(i_args.ri_sides, 3 * sizeof(ZPoint<1>) /* offset */);
      
      ZPoint<1> pz = is_zones.bounds.lo;
      ZPoint<1> ps = is_sides.bounds.lo;

      for(int zy = zylo; zy < zyhi; zy++) {
	for(int zx = zxlo; zx < zxhi; zx++) {
	  // get 4 side pointers
	  ZPoint<1> ps0 = ps; ps.x++;
	  ZPoint<1> ps1 = ps; ps.x++;
	  ZPoint<1> ps2 = ps; ps.x++;
	  ZPoint<1> ps3 = ps; ps.x++;

	  // point pointers are ugly because they can be in neighbors - use a helper
	  ZPoint<1> pp0 = global_point_pointer(zy, zx); // go CCW
	  ZPoint<1> pp1 = global_point_pointer(zy+1, zx);
	  ZPoint<1> pp2 = global_point_pointer(zy+1, zx+1);
	  ZPoint<1> pp3 = global_point_pointer(zy, zx+1);

	  a_zone_color.write(pz, i_args.index);

	  a_side_mapsz.write(ps0, pz);
	  a_side_mapsz.write(ps1, pz);
	  a_side_mapsz.write(ps2, pz);
	  a_side_mapsz.write(ps3, pz);

	  a_side_mapss3.write(ps0, ps1);
	  a_side_mapss3.write(ps1, ps2);
	  a_side_mapss3.write(ps2, ps3);
	  a_side_mapss3.write(ps3, ps0);

	  a_side_mapsp1.write(ps0, pp0);
	  a_side_mapsp1.write(ps1, pp1);
	  a_side_mapsp1.write(ps2, pp2);
	  a_side_mapsp1.write(ps3, pp3);

	  a_side_ok.write(ps0, true);
	  a_side_ok.write(ps1, true);
	  a_side_ok.write(ps2, true);
	  a_side_ok.write(ps3, true);

	  pz.x++;
	}
      }
      assert(pz.x == is_zones.bounds.hi.x + 1);
      assert(ps.x == is_sides.bounds.hi.x + 1);
    }
    
    if(show_graph) {
      AffineAccessor<int,1> a_zone_color(i_args.ri_zones, 0 /* offset */);

      for(int i = is_zones.bounds.lo; i <= is_zones.bounds.hi; i++)
	std::cout << "Z[" << i << "]: color=" << a_zone_color.read(i) << std::endl;

      AffineAccessor<ZPoint<1>,1> a_side_mapsz(i_args.ri_sides, 0 * sizeof(ZPoint<1>) /* offset */);
      AffineAccessor<ZPoint<1>,1> a_side_mapss3(i_args.ri_sides, 1 * sizeof(ZPoint<1>) /* offset */);
      AffineAccessor<ZPoint<1>,1> a_side_mapsp1(i_args.ri_sides, 2 * sizeof(ZPoint<1>) /* offset */);
      AffineAccessor<bool,1> a_side_ok(i_args.ri_sides, 3 * sizeof(ZPoint<1>) /* offset */);

      for(int i = is_sides.bounds.lo; i <= is_sides.bounds.hi; i++)
	std::cout << "S[" << i << "]:"
		  << " mapsz=" << a_side_mapsz.read(i)
		  << " mapss3=" << a_side_mapss3.read(i)
		  << " mapsp1=" << a_side_mapsp1.read(i)
		  << " ok=" << a_side_ok.read(i)
		  << std::endl;
    }
  }

  // the outputs of our partitioning will be:
  //  p_zones               - subsets of is_zones split by piece
  //  p_sides               - subsets of is_sides split by piece (with bad sides removed)
  //  p_points              - subsets of is_points by piece (aliased)

  std::vector<ZIndexSpace<1> > p_zones;
  std::vector<ZIndexSpace<1> > p_sides;
  std::vector<ZIndexSpace<1> > p_points;

  virtual Event perform_partitioning(void)
  {
    // first get the set of bad sides (i.e. ok == false)
    ZIndexSpace<1> bad_sides;

    Event e1 = is_sides.create_subspace_by_field(side_ok_field_data,
						 false,
						 bad_sides,
						 Realm::ProfilingRequestSet());
    if(wait_on_events) e1.wait();

    // map the bad sides through to bad zones
    ZIndexSpace<1> bad_zones;
    Event e2 = is_zones.create_subspace_by_image(side_mapsz_field_data,
						 bad_sides,
						 bad_zones,
						 Realm::ProfilingRequestSet(),
						 e1);
    if(wait_on_events) e2.wait();

    // subtract bad zones to get good zones
    ZIndexSpace<1> good_zones;
    Event e3 = ZIndexSpace<1>::compute_difference(is_zones, bad_zones, good_zones,
						  Realm::ProfilingRequestSet(),
						  e2);
    if(wait_on_events) e3.wait();

    // now do actual partitions with just good zones
    std::vector<int> colors(numpc);
    for(int i = 0; i < numpc; i++)
      colors[i] = i;

    Event e4 = good_zones.create_subspaces_by_field(zone_color_field_data,
						    colors,
						    p_zones,
						    Realm::ProfilingRequestSet(),
						    e3);
    if(wait_on_events) e4.wait();

    // preimage of zones is sides
    Event e5 = is_sides.create_subspaces_by_preimage(side_mapsz_field_data,
						     p_zones,
						     p_sides,
						     Realm::ProfilingRequestSet(),
						     e4);
    if(wait_on_events) e5.wait();

    // and image of sides->mapsp1 is points
    Event e6 = is_points.create_subspaces_by_image(side_mapsp1_field_data,
						   p_sides,
						   p_points,
						   Realm::ProfilingRequestSet(),
						   e5);
    if(wait_on_events) e6.wait();

    return e6;
  }

  virtual int check_partitioning(void)
  {
    int errors = 0;

    for(int pcy = 0; pcy < numpcy; pcy++) {
      for(int pcx = 0; pcx < numpcx; pcx++) {
	int idx = pcy * numpcx + pcx;

	int lx = zxbound[pcx + 1] - zxbound[pcx];
	int ly = zybound[pcy + 1] - zybound[pcy];

	int exp_zones = lx * ly;
	int exp_sides = exp_zones * 4;
	int exp_points = (lx + 1) * (ly + 1); // easier because of aliasing

	int act_zones = p_zones[idx].volume();
	int act_sides = p_sides[idx].volume();
	int act_points = p_points[idx].volume();

	if(exp_zones != act_zones) {
	  log_app.error() << "Piece #" << idx << ": zone count mismatch: exp = " << exp_zones << ", act = " << act_zones;
	  errors++;
	}
	if(exp_sides != act_sides) {
	  log_app.error() << "Piece #" << idx << ": side count mismatch: exp = " << exp_sides << ", act = " << act_sides;
	  errors++;
	}
	if(exp_points != act_points) {
	  log_app.error() << "Piece #" << idx << ": point count mismatch: exp = " << exp_points << ", act = " << act_points;
	  errors++;
	}
      }
    }

    // check zones
    ZPoint<1> pz = is_zones.bounds.lo;
    for(int pc = 0; pc < numpc; pc++) {
      for(int i = 0; i < lz[pc]; i++) {
	for(int j = 0; j < numpc; j++) {
	  bool exp = (j == pc);
	  bool act = p_zones[j].contains(pz);
	  if(exp != act) {
	    log_app.error() << "mismatch: zone " << pz << " in p_zones[" << j << "]: exp=" << exp << " act=" << act;
	    errors++;
	  }
	}
	pz.x++;
      }
    }

    // check sides
    ZPoint<1> ps = is_sides.bounds.lo;
    for(int pc = 0; pc < numpc; pc++) {
      for(int i = 0; i < ls[pc]; i++) {
	for(int j = 0; j < numpc; j++) {
	  bool exp = (j == pc);
	  bool act = p_sides[j].contains(ps);
	  if(exp != act) {
	    log_app.error() << "mismatch: side " << ps << " in p_sides[" << j << "]: exp=" << exp << " act=" << act;
	    errors++;
	  }
	}
	ps.x++;
      }
    }

    // check points (trickier due to ghosting)
    for(int py = 0; py < npy; py++)
      for(int px = 0; px < npx; px++) {
	ZPoint<1> pp = global_point_pointer(py, px);
	for(int pc = 0; pc < numpc; pc++) {
	  int pcy = pc / numpcx;
	  int pcx = pc % numpcx;
	  bool exp = ((py >= zybound[pcy]) && (py <= zybound[pcy + 1]) &&
		      (px >= zxbound[pcx]) && (px <= zxbound[pcx + 1]));
	  bool act = p_points[pc].contains(pp);
	  if(exp != act) {
	    log_app.error() << "mismatch: point " << pp << " in p_points[" << pc << "]: exp=" << exp << " act=" << act;
	    errors++;
	  }
	}
      }
    
    return errors;
  }
};


void top_level_task(const void *args, size_t arglen, Processor p)
{
  int errors = 0;

  testcfg->print_info();

  // find all the system memories - we'll stride our data across them
  // for each memory, we'll need one CPU that can do the initialization of the data
  std::vector<Memory> sysmems;
  std::vector<Processor> procs;

  Machine machine = Machine::get_machine();
  {
    std::set<Memory> all_memories;
    machine.get_all_memories(all_memories);
    for(std::set<Memory>::const_iterator it = all_memories.begin();
	it != all_memories.end();
	it++) {
      Memory m = *it;
      if(m.kind() == Memory::SYSTEM_MEM) {
	sysmems.push_back(m);
	std::set<Processor> pset;
	machine.get_shared_processors(m, pset);
	Processor p = Processor::NO_PROC;
	for(std::set<Processor>::const_iterator it2 = pset.begin();
	    it2 != pset.end();
	    it2++) {
	  if(it2->kind() == Processor::LOC_PROC) {
	    p = *it2;
	    break;
	  }
	}
	assert(p.exists());
	procs.push_back(p);
	log_app.debug() << "System mem #" << (sysmems.size() - 1) << " = " << *sysmems.rbegin() << " (" << *procs.rbegin() << ")";
      }
    }
  }
  assert(sysmems.size() > 0);

  {
    Realm::TimeStamp ts("initialization", true, &log_app);
  
    Event e = testcfg->initialize_data(sysmems, procs);
    // wait for all initialization to be done
    e.wait();
  }

  // now actual partitioning work
  {
    Realm::TimeStamp ts("dependent partitioning work", true, &log_app);

    Event e = testcfg->perform_partitioning();

    e.wait();
  }

  if(!skip_check) {
    log_app.print() << "checking correctness of partitioning";
    Realm::TimeStamp ts("verification", true, &log_app);
    errors += testcfg->check_partitioning();
  }

  if(errors > 0) {
    printf("Exiting with errors\n");
    exit(1);
  }

  printf("all done!\n");
  sleep(1);

  Runtime::get_runtime().shutdown();
}

int main(int argc, char **argv)
{
  Runtime rt;

  rt.init(&argc, &argv);

  // parse global options
  for(int i = 1; i < argc; i++) {
    if(!strcmp(argv[i], "-seed")) {
      random_seed = atoi(argv[++i]);
      continue;
    }

    if(!strcmp(argv[i], "-random")) {
      random_colors = true;
      continue;
    }

    if(!strcmp(argv[i], "-wait")) {
      wait_on_events = true;
      continue;
    }

    if(!strcmp(argv[i], "-show")) {
      show_graph = true;
      continue;
    }

    if(!strcmp(argv[i], "-nocheck")) {
      skip_check = true;
      continue;
    }

    // test cases consume the rest of the args
    if(!strcmp(argv[i], "circuit")) {
      testcfg = new CircuitTest(argc-i, const_cast<const char **>(argv+i));
      break;
    }
  
    if(!strcmp(argv[i], "pennant")) {
      testcfg = new PennantTest(argc-i, const_cast<const char **>(argv+i));
      break;
    }
  
    if(!strcmp(argv[i], "miniaero")) {
      testcfg = new MiniAeroTest(argc-i, const_cast<const char **>(argv+i));
      break;
    }

    printf("unknown parameter: %s\n", argv[i]);
  }

  // if no test specified, use circuit (with default parameters)
  if(!testcfg)
    testcfg = new CircuitTest(0, 0);

  rt.register_task(TOP_LEVEL_TASK, top_level_task);
  rt.register_task(INIT_CIRCUIT_DATA_TASK, CircuitTest::init_data_task_wrapper);
  rt.register_task(INIT_PENNANT_DATA_TASK, PennantTest::init_data_task_wrapper);
  rt.register_task(INIT_MINIAERO_DATA_TASK, MiniAeroTest::init_data_task_wrapper);

  signal(SIGALRM, sigalrm_handler);

  // Start the machine running
  // Control never returns from this call
  // Note we only run the top level task on one processor
  // You can also run the top level task on all processors or one processor per node
  rt.run(TOP_LEVEL_TASK, Runtime::ONE_TASK_ONLY);

  //rt.shutdown();
  return 0;
}
