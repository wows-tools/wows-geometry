int vertex2id(const char *vertex_type);
const char *id2vertex(int id);
uint16_t datatoh16(char *data, size_t offset, WOWS_GEOMETRY_CONTEXT *context);
uint32_t datatoh32(char *data, size_t offset, WOWS_GEOMETRY_CONTEXT *context);
uint64_t datatoh64(char *data, size_t offset, WOWS_GEOMETRY_CONTEXT *context);
void normalise(float *x, float *y, float *z);
float clamp(float min, float value, float max);
int wows_unpack_normal_old(wows_vertex *vertex_packed);
int wows_pack_normal_old(wows_vertex *vertex_packed);
