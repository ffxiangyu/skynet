#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include "msvcint.h"

#include "sproto.h"

#define CHUNK_SIZE 1000
#define SIZEOF_LENGTH 4
#define SIZEOF_HEADER 2
#define SIZEOF_FIELD 2

size_t sizeof_sproto_simple_type(int type)
{
	switch (type) {
	case SPROTO_TINTEGER:   // int64
		return SIZEOF_INT64;
	default:
		fprintf(stderr, "sizeof_sproto_simple_type unsupported type %d\n", type);
	}
	return 0;
}

static void
pool_init(struct pool *p) {
	p->header = NULL;
	p->current = NULL;
	p->current_used = 0;
}

static void
pool_release(struct pool *p) {
	struct chunk * tmp = p->header;
	while (tmp) {
		struct chunk * n = tmp->next;
		free(tmp);
		tmp = n;
	}
}

static void *
pool_newchunk(struct pool *p, size_t sz) {
	struct chunk * t = malloc(sz + sizeof(struct chunk));
	if (t == NULL)
		return NULL;
	t->next = p->header;
	p->header = t;
	return t + 1;
}

static void *
pool_alloc(struct pool *p, size_t sz) {
	// align by 8
	sz = (sz + 7) & ~7;
	if (sz >= CHUNK_SIZE) {
		return pool_newchunk(p, sz);
	}
	if (p->current == NULL) {
		if (pool_newchunk(p, CHUNK_SIZE) == NULL)
			return NULL;
		p->current = p->header;
	}
	if (sz + p->current_used <= CHUNK_SIZE) {
		void * ret = (char *)(p->current + 1) + p->current_used;
		p->current_used += sz;
		return ret;
	}

	if (sz >= p->current_used) {
		return pool_newchunk(p, sz);
	}
	else {
		void * ret = pool_newchunk(p, CHUNK_SIZE);
		p->current = p->header;
		p->current_used = sz;
		return ret;
	}
}

static inline int
toword(const uint8_t * p) {
	return p[0] | p[1] << 8;
}

static inline uint32_t
todword(const uint8_t *p) {
	return p[0] | p[1] << 8 | p[2] << 16 | p[3] << 24;
}

static int
count_array(const uint8_t * stream) {
	uint32_t length = todword(stream);
	int n = 0;
	stream += SIZEOF_LENGTH;
	while (length > 0) {
		uint32_t nsz;
		if (length < SIZEOF_LENGTH)
			return -1;
		nsz = todword(stream);
		nsz += SIZEOF_LENGTH;
		if (nsz > length)
			return -1;
		++n;
		stream += nsz;
		length -= nsz;
	}

	return n;
}

static int
struct_field(const uint8_t * stream, size_t sz) {
	const uint8_t * field;
	int fn, header, i;
	if (sz < SIZEOF_LENGTH)
		return -1;
	fn = toword(stream);
	header = SIZEOF_HEADER + SIZEOF_FIELD * fn;
	if (sz < header)
		return -1;
	field = stream + SIZEOF_HEADER;
	sz -= header;
	stream += header;
	for (i = 0; i<fn; i++) {
		int value = toword(field + i * SIZEOF_FIELD);
		uint32_t dsz;
		if (value != 0)
			continue;
		if (sz < SIZEOF_LENGTH)
			return -1;
		dsz = todword(stream);
		if (sz < SIZEOF_LENGTH + dsz)
			return -1;
		stream += SIZEOF_LENGTH + dsz;
		sz -= SIZEOF_LENGTH + dsz;
	}

	return fn;
}

static const char *
import_string(struct sproto *s, const uint8_t * stream) {
	uint32_t sz = todword(stream);
	char * buffer = pool_alloc(&s->memory, sz + 1);
	memcpy(buffer, stream + SIZEOF_LENGTH, sz);
	buffer[sz] = '\0';
	return buffer;
}

static int
calc_pow(int base, int n) {
	int r;
	if (n == 0)
		return 1;
	r = calc_pow(base * base, n / 2);
	if (n & 1) {
		r *= base;
	}
	return r;
}

static const uint8_t *
import_field(struct sproto *s, struct sproto_field *f, const uint8_t * stream) {
	uint32_t sz;
	const uint8_t * result;
	int fn;
	int i;
	int array = 0;
	int tag = -1;
	f->tag = -1;
	f->type = -1;
	f->name = NULL;
	f->st = NULL;
	f->key = -1;
	f->extra = 0;

	sz = todword(stream);
	stream += SIZEOF_LENGTH;
	result = stream + sz;
	fn = struct_field(stream, sz);
	if (fn < 0)
		return NULL;
	stream += SIZEOF_HEADER;
	for (i = 0; i<fn; i++) {
		int value;
		++tag;
		value = toword(stream + SIZEOF_FIELD * i);
		if (value & 1) {
			tag += value / 2;
			continue;
		}
		if (tag == 0) { // name
			if (value != 0)
				return NULL;
			f->name = import_string(s, stream + fn * SIZEOF_FIELD);
			continue;
		}
		if (value == 0)
			return NULL;
		value = value / 2 - 1;
		switch (tag) {
		case 1: // buildin
			if (value >= SPROTO_TSTRUCT)
				return NULL;	// invalid buildin type
			f->type = value;
			break;
		case 2: // type index
			if (f->type == SPROTO_TINTEGER) {
				f->extra = calc_pow(10, value);
			}
			else if (f->type == SPROTO_TSTRING) {
				f->extra = value;	// string if 0 ; binary is 1
			}
			else {
				if (value >= s->type_n)
					return NULL;	// invalid type index
				if (f->type >= 0)
					return NULL;
				f->type = SPROTO_TSTRUCT;
				f->st = &s->type[value];
			}
			break;
		case 3: // tag
			f->tag = value;
			break;
		case 4: // array
			if (value)
				array = SPROTO_TARRAY;
			break;
		case 5:	// key
			f->key = value;
			break;
		default:
			return NULL;
		}
	}
	if (f->tag < 0 || f->type < 0 || f->name == NULL)
		return NULL;
	f->type |= array;

	return result;
}

static bool is_unit_for_arr_field(const char *name) {
	size_t name_len = strlen(name);
	if (name_len <= 13) // _unit_for_arr 's len is 13
		return false;
	char subbuff[14];
	memcpy(subbuff, name + name_len - 13, 13);
	subbuff[13] = '\0';
	return 0 == strcmp(subbuff, "_unit_for_arr");
}
/*
.type {
.field {
name 0 : string
buildin 1 : integer
type 2 : integer
tag 3 : integer
array 4 : boolean
}
name 0 : string
fields 1 : *field
}
*/
static const uint8_t *
import_type(struct sproto *s, struct sproto_type *t, const uint8_t * stream) {
	const uint8_t * result;
	uint32_t sz = todword(stream);
	int i;
	int fn;
	int n;
	int maxn;
	int last;
	stream += SIZEOF_LENGTH;
	result = stream + sz;
	fn = struct_field(stream, sz);
	if (fn <= 0 || fn > 2)
		return NULL;
	for (i = 0; i<fn*SIZEOF_FIELD; i += SIZEOF_FIELD) {
		// name and fields must encode to 0
		int v = toword(stream + SIZEOF_HEADER + i);
		if (v != 0)
			return NULL;
	}
	memset(t, 0, sizeof(*t));
	stream += SIZEOF_HEADER + fn * SIZEOF_FIELD;
	t->name = import_string(s, stream);
	if (fn == 1) {
		return result;
	}
	stream += todword(stream) + SIZEOF_LENGTH;	// second data
	n = count_array(stream);
	if (n<0)
		return NULL;
	stream += SIZEOF_LENGTH;
#ifdef SPROTO_OBJ
	int j, k;
	int c_struct_offset = 0;
	int num_arr = 0;
	int sproto_obj_n = n;
	bool suc = true;
#endif
	maxn = n;
	last = -1;
	t->n = n;
	t->f = pool_alloc(&s->memory, sizeof(struct sproto_field) * n);
	for (i = 0; i<n; i++) {
		int tag;
		struct sproto_field *f = &t->f[i];
		stream = import_field(s, f, stream);
		if (stream == NULL)
			return NULL;
		fprintf(stderr, "  import_field %d  %10s\n", i, t->f[i].name);
		tag = f->tag;
#ifdef SPROTO_OBJ
		f->offset_for_obj = c_struct_offset;
		f->unit_for_arr = 0;
		f->arr_idx = 0;
		if (is_unit_for_arr_field(f->name))
			;
		else if (SPROTO_TINTEGER == f->type)
			c_struct_offset += SIZEOF_INT64;
		else if (SPROTO_TBOOLEAN == f->type)
			c_struct_offset += SIZEOF_BOOL;
		else if (SPROTO_TSTRING == f->type)
			c_struct_offset += SIZEOF_POINTER;
		else if (SPROTO_TSTRUCT == f->type) {
			if (f->st->n < 0)
				suc = false;
			else {
				c_struct_offset += f->st->size_for_obj_no_arr; // this struct is full data
				num_arr += f->st->num_arr;
				sproto_obj_n += f->st->n - 1;
			}
		}
		else if (SPROTO_TARRAY & f->type) {
			num_arr++;
			f->unit_for_arr = 1; // default 1 for array, if no xx_unit_for_arr field
		}
		else {
			fprintf(stderr, "sproto.c import_type ERROR unsupported field type %d", f->type); // error
			exit(1);
		}
#endif
		if (!is_unit_for_arr_field(f->name) && tag <= last)
			return NULL;	// tag must in ascending order, for bi-search
		if (tag > last + 1) {
			++maxn;
		}
		last = tag;
	}
	t->maxn = maxn;
	t->base = t->f[0].tag;
	n = t->f[n - 1].tag - t->base + 1;
	if (n != t->n) {
		t->base = -1;
	}
#ifdef SPROTO_OBJ
	t->num_arr = num_arr;
	t->size_for_obj_no_arr = c_struct_offset;
	if (suc &&
		'_' == t->name[0] && '_' == t->name[strlen(t->name) - 1]) { // world object

		t->base = -1;
		fprintf(stderr, "sproto world obj %s :\n", t->name);
		for (i = 0; i < t->n; i++)
			fprintf(stderr, "  %d %10s type %d\n", i, t->f[i].name, t->f[i].type);

		// unit_for_arr
		for (i = 0; i < t->n; i++) {
			struct sproto_field *f = &t->f[i];
			if (!is_unit_for_arr_field(f->name))
				continue;
			fprintf(stderr, "  unit_for_arr field %s :\n", f->name);
			size_t name_len = strlen(f->name);
			char *pre_buf;
			pre_buf = (char*)malloc((name_len - 13 + 1) * sizeof(char));
			memcpy(pre_buf, f->name, name_len - 13);
			pre_buf[name_len - 13] = '\0';
			for (j = 0; j < t->n; j++) {
				struct sproto_field *f_arr = &t->f[j];
				if (0 == (SPROTO_TARRAY & f_arr->type))
					continue;
				if (0 != strcmp(pre_buf, f_arr->name))
					continue;
				f_arr->unit_for_arr = f->tag;
				fprintf(stderr, "  unit_for_arr %s %d\n", f_arr->name, f_arr->unit_for_arr);
				break;
			}
			sproto_obj_n--; // will not need *unit* field
			f->unit_for_arr = -1; // to skip the *unit* field later

			free(pre_buf);
		}

		struct sproto_field *tmp = pool_alloc(&s->memory, sizeof(struct sproto_field) * sproto_obj_n);
		int tag_add = 0;
		for (i = 0; i < t->n; i++) {
			struct sproto_field *f = &t->f[i];
			if (SPROTO_TSTRUCT != f->type)
				continue;
			for (j = 0; j < f->st->n; j++) {
				if (f->st->f[j].tag > tag_add)
					tag_add = f->st->f[j].tag;
			}
		}
		if (tag_add > 0)
			tag_add = (tag_add / 1000 + 1) * 1000;
		// WorldObj's tag_add must be 0
		;
		int arr_idx = 0;
		for (i = 0, j = 0; i < t->n; i++) {
			struct sproto_field *f = &t->f[i];
			if (SPROTO_TSTRUCT == f->type) {
				// fprintf(stderr, " struct %s %d fields\n", f->st->name, f->st->n);
				for (k = 0; k < f->st->n; k++) {
					tmp[j].offset_for_obj = f->offset_for_obj + f->st->f[k].offset_for_obj;
					if (SPROTO_TARRAY & tmp[j].type)
						tmp[j].arr_idx = arr_idx++;
					tmp[j++] = f->st->f[k];
					// fprintf(stderr, "  %d %10s %d %d\n", j+k, tmp[j+k].name, tmp[j+k].type, f->st->f[k].type);
				}
			}
			else {
				if (f->unit_for_arr < 0)
					continue;
				tmp[j] = t->f[i];
				if (SPROTO_TARRAY & tmp[j].type)
					tmp[j].arr_idx = arr_idx++;
				tmp[j++].tag = t->f[i].tag + tag_add;
				// fprintf(stderr, "  %d %10s %d %d\n", j-1, tmp[j-1].name, tmp[j-1].type, t->f[i].type);
			}
		}
		t->n = sproto_obj_n;
		t->f = tmp;

		// log
		for (i = 0; i < t->n; i++) {
			fprintf(stderr, " %3d ", i);
			struct sproto_field *f = &t->f[i];
			fprintf(stderr, " %16s %6d  type %5d %d", f->name, f->tag, f->type, (int)f->offset_for_obj);
			if (SPROTO_TARRAY & f->type)
				fprintf(stderr, "  unit %3d", (int)f->unit_for_arr);
			fprintf(stderr, "\n");
		}
	}
	if (!suc)
		t->n = -1;
	fprintf(stderr, "import_type %s %s\n", t->name, suc ? "suc" : "fail");
#endif
	return result;
}

/*
.protocol {
name 0 : string
tag 1 : integer
request 2 : integer
response 3 : integer
}
*/
static const uint8_t *
import_protocol(struct sproto *s, struct protocol *p, const uint8_t * stream) {
	const uint8_t * result;
	uint32_t sz = todword(stream);
	int fn;
	int i;
	int tag;
	stream += SIZEOF_LENGTH;
	result = stream + sz;
	fn = struct_field(stream, sz);
	stream += SIZEOF_HEADER;
	p->name = NULL;
	p->tag = -1;
	p->p[SPROTO_REQUEST] = NULL;
	p->p[SPROTO_RESPONSE] = NULL;
	p->confirm = 0;
	tag = 0;
	for (i = 0; i<fn; i++, tag++) {
		int value = toword(stream + SIZEOF_FIELD * i);
		if (value & 1) {
			tag += (value - 1) / 2;
			continue;
		}
		value = value / 2 - 1;
		switch (i) {
		case 0: // name
			if (value != -1) {
				return NULL;
			}
			p->name = import_string(s, stream + SIZEOF_FIELD * fn);
			break;
		case 1: // tag
			if (value < 0) {
				return NULL;
			}
			p->tag = value;
			break;
		case 2: // request
			if (value < 0 || value >= s->type_n)
				return NULL;
			p->p[SPROTO_REQUEST] = &s->type[value];
			break;
		case 3: // response
			if (value < 0 || value >= s->type_n)
				return NULL;
			p->p[SPROTO_RESPONSE] = &s->type[value];
			break;
		case 4:	// confirm
			p->confirm = value;
			break;
		default:
			return NULL;
		}
	}

	if (p->name == NULL || p->tag<0) {
		return NULL;
	}

	return result;
}

static struct sproto *
create_from_bundle(struct sproto *s, const uint8_t * stream, size_t sz) {
	const uint8_t * content;
	const uint8_t * typedata = NULL;
	const uint8_t * protocoldata = NULL;
	int fn = struct_field(stream, sz);
	fprintf(stderr, "create_from_bundle fn %d\n", fn);
	int i;
	if (fn < 0 || fn > 2)
		return NULL;

	stream += SIZEOF_HEADER;
	content = stream + fn * SIZEOF_FIELD;

	for (i = 0; i<fn; i++) {
		int value = toword(stream + i * SIZEOF_FIELD);
		fprintf(stderr, "create_from_bundle %d :\n", i);
		fprintf(stderr, "create_from_bundle %d\n", value);
		int n;
		if (value != 0)
			return NULL;
		n = count_array(content);
		fprintf(stderr, "create_from_bundle %d\n", n);
		if (n<0)
			return NULL;
		if (i == 0) {
			typedata = content + SIZEOF_LENGTH;
			s->type_n = n;
			s->type = pool_alloc(&s->memory, n * sizeof(*s->type));
		}
		else {
			protocoldata = content + SIZEOF_LENGTH;
			s->protocol_n = n;
			s->proto = pool_alloc(&s->memory, n * sizeof(*s->proto));
		}
		content += todword(content) + SIZEOF_LENGTH;
	}

	int j;
	const uint8_t * typedataCpy = typedata;
	for (i = 0; i < s->type_n; i++)
		s->type[i].n = -1; // not successfully imported
	bool suc;
	for (j = 1; j < 999; j++) {
		typedata = typedataCpy;
		fprintf(stderr, "create_from_bundle import types loop %d\n", j);
		suc = true;
		for (i = 0; i < s->type_n; i++) {
			//if (s->type[i].n >= 0) // cant skip, buffer proccessing
			//	continue;
			typedata = import_type(s, &s->type[i], typedata);
			if (typedata == NULL) {
				fprintf(stderr, "typedata == NULL\n");
				return NULL;
			}
			if (s->type[i].n < 0)
				suc = false;
		}
		if (suc)
			break;
	}
	fprintf(stderr, "create_from_bundle import types finish\n");
	for (i = 0; i<s->protocol_n; i++) {
		protocoldata = import_protocol(s, &s->proto[i], protocoldata);
		if (protocoldata == NULL) {
			fprintf(stderr, "protocoldata == NULL\n");
			return NULL;
		}
	}

	fprintf(stderr, "create_from_bundle return s\n");
	return s;
}

struct sproto *
	sproto_create(const void * proto, size_t sz) {
	struct pool mem;
	struct sproto * s;
	pool_init(&mem);
	s = pool_alloc(&mem, sizeof(*s));
	if (s == NULL)
		return NULL;
	memset(s, 0, sizeof(*s));
	s->memory = mem;
	if (create_from_bundle(s, proto, sz) == NULL) {
		pool_release(&s->memory);
		return NULL;
	}
	return s;
}

void
sproto_release(struct sproto * s) {
	if (s == NULL)
		return;
	pool_release(&s->memory);
}

void
sproto_dump(struct sproto *s) {
	int i, j;
	fprintf(stderr, "=== %d types ===\n", s->type_n);
	for (i = 0; i<s->type_n; i++) {
		struct sproto_type *t = &s->type[i];
		fprintf(stderr, "%s\n", t->name);
		for (j = 0; j<t->n; j++) {
			char array[2] = { 0, 0 };
			const char * type_name = NULL;
			struct sproto_field *f = &t->f[j];
			int type = f->type & ~SPROTO_TARRAY;
			if (f->type & SPROTO_TARRAY) {
				array[0] = '*';
			}
			else {
				array[0] = 0;
			}
			if (type == SPROTO_TSTRUCT) {
				type_name = f->st->name;
			}
			else {
				switch (type) {
				case SPROTO_TINTEGER:
					if (f->extra) {
						type_name = "decimal";
					}
					else {
						type_name = "integer";
					}
					break;
				case SPROTO_TBOOLEAN:
					type_name = "boolean";
					break;
				case SPROTO_TSTRING:
					if (f->extra == SPROTO_TSTRING_BINARY)
						type_name = "binary";
					else
						type_name = "string";
					break;
				default:
					type_name = "invalid";
					break;
				}
			}
			printf("\t%s (%d) %s%s", f->name, f->tag, array, type_name);
			if (type == SPROTO_TINTEGER && f->extra > 0) {
				printf("(%d)", f->extra);
			}
			if (f->key >= 0) {
				printf("[%d]", f->key);
			}
			printf("\n");
		}
	}
	printf("=== %d protocol ===\n", s->protocol_n);
	for (i = 0; i<s->protocol_n; i++) {
		struct protocol *p = &s->proto[i];
		if (p->p[SPROTO_REQUEST]) {
			printf("\t%s (%d) request:%s", p->name, p->tag, p->p[SPROTO_REQUEST]->name);
		}
		else {
			printf("\t%s (%d) request:(null)", p->name, p->tag);
		}
		if (p->p[SPROTO_RESPONSE]) {
			printf(" response:%s", p->p[SPROTO_RESPONSE]->name);
		}
		else if (p->confirm) {
			printf(" response nil");
		}
		printf("\n");
	}
}

// query
int
sproto_prototag(const struct sproto *sp, const char * name) {
	int i;
	for (i = 0; i<sp->protocol_n; i++) {
		if (strcmp(name, sp->proto[i].name) == 0) {
			return sp->proto[i].tag;
		}
	}
	return -1;
}

static struct protocol *
query_proto(const struct sproto *sp, int tag) {
	int begin = 0, end = sp->protocol_n;
	while (begin<end) {
		int mid = (begin + end) / 2;
		int t = sp->proto[mid].tag;
		if (t == tag) {
			return &sp->proto[mid];
		}
		if (tag > t) {
			begin = mid + 1;
		}
		else {
			end = mid;
		}
	}
	return NULL;
}

struct sproto_type *
	sproto_protoquery(const struct sproto *sp, int proto, int what) {
	struct protocol * p;
	if (what <0 || what >1) {
		return NULL;
	}
	p = query_proto(sp, proto);
	if (p) {
		return p->p[what];
	}
	return NULL;
}

int
sproto_protoresponse(const struct sproto * sp, int proto) {
	struct protocol * p = query_proto(sp, proto);
	return (p != NULL && (p->p[SPROTO_RESPONSE] || p->confirm));
}

const char *
sproto_protoname(const struct sproto *sp, int proto) {
	struct protocol * p = query_proto(sp, proto);
	if (p) {
		return p->name;
	}
	return NULL;
}

struct sproto_type *
	sproto_type(const struct sproto *sp, const char * type_name) {
	int i;
	for (i = 0; i<sp->type_n; i++) {
		if (strcmp(type_name, sp->type[i].name) == 0) {
			return &sp->type[i];
		}
	}
	return NULL;
}

const char *
sproto_name(struct sproto_type * st) {
	return st->name;
}

static struct sproto_field *
findtag(const struct sproto_type *st, int tag) {
	int begin, end;
	if (st->base >= 0) {
		tag -= st->base;
		if (tag < 0 || tag >= st->n)
			return NULL;
		return &st->f[tag];
	}
	begin = 0;
	end = st->n;
	while (begin < end) {
		int mid = (begin + end) / 2;
		struct sproto_field *f = &st->f[mid];
		int t = f->tag;
		if (t == tag) {
			return f;
		}
		if (tag > t) {
			begin = mid + 1;
		}
		else {
			end = mid;
		}
	}
	return NULL;
}

struct sproto_field *
	sproto_field(const struct sproto_type *st, const char * field_name) {
	int i;
	for (i = 0; i < st->n; i++) {
		if (strcmp(field_name, st->f[i].name) == 0) {
			return &st->f[i];
		}
	}
	return NULL;
}

struct sproto_field *
	sproto_field_by_tag(const struct sproto_type *st, int tag) {
	return findtag(st, tag);
}

// encode & decode
// sproto_callback(void *ud, int tag, int type, struct sproto_type *, void *value, int length)
//	  return size, -1 means error

static inline int
fill_size(uint8_t * data, int sz) {
	data[0] = sz & 0xff;
	data[1] = (sz >> 8) & 0xff;
	data[2] = (sz >> 16) & 0xff;
	data[3] = (sz >> 24) & 0xff;
	return sz + SIZEOF_LENGTH;
}

static int
encode_integer(uint32_t v, uint8_t * data, int size) {
	if (size < SIZEOF_LENGTH + sizeof(v))
		return -1;
	data[4] = v & 0xff;
	data[5] = (v >> 8) & 0xff;
	data[6] = (v >> 16) & 0xff;
	data[7] = (v >> 24) & 0xff;
	return fill_size(data, sizeof(v));
}

static int
encode_uint64(uint64_t v, uint8_t * data, int size) {
	if (size < SIZEOF_LENGTH + sizeof(v))
		return -1;
	data[4] = v & 0xff;
	data[5] = (v >> 8) & 0xff;
	data[6] = (v >> 16) & 0xff;
	data[7] = (v >> 24) & 0xff;
	data[8] = (v >> 32) & 0xff;
	data[9] = (v >> 40) & 0xff;
	data[10] = (v >> 48) & 0xff;
	data[11] = (v >> 56) & 0xff;
	return fill_size(data, sizeof(v));
}

/*
//#define CB(tagname,type,index,subtype,value,length) cb(ud, tagname,type,index,subtype,value,length)

static int
do_cb(sproto_callback cb, void *ud, const char *tagname, int type, int index, struct sproto_type *subtype, void *value, int length) {
if (subtype) {
if (type >= 0) {
printf("callback: tag=%s[%d], subtype[%s]:%d\n",tagname,index, subtype->name, type);
} else {
printf("callback: tag=%s[%d], subtype[%s]\n",tagname,index, subtype->name);
}
} else if (index > 0) {
printf("callback: tag=%s[%d]\n",tagname,index);
} else if (index == 0) {
printf("callback: tag=%s\n",tagname);
} else {
printf("callback: tag=%s [mainkey]\n",tagname);
}
return cb(ud, tagname,type,index,subtype,value,length);
}
#define CB(tagname,type,index,subtype,value,length) do_cb(cb,ud, tagname,type,index,subtype,value,length)
*/

static int
encode_object(sproto_callback cb, struct sproto_arg *args, uint8_t *data, int size) {
	int sz;
	if (size < SIZEOF_LENGTH)
		return -1;
	args->value = data + SIZEOF_LENGTH;
	args->length = size - SIZEOF_LENGTH;
	sz = cb(args);
	if (sz < 0) {
		if (sz == SPROTO_CB_NIL)
			return 0;
		return -1;	// sz == SPROTO_CB_ERROR
	}
	assert(sz <= size - SIZEOF_LENGTH);	// verify buffer overflow
	return fill_size(data, sz);
}

static inline void
uint32_to_uint64(int negative, uint8_t *buffer) {
	if (negative) {
		buffer[4] = 0xff;
		buffer[5] = 0xff;
		buffer[6] = 0xff;
		buffer[7] = 0xff;
	}
	else {
		buffer[4] = 0;
		buffer[5] = 0;
		buffer[6] = 0;
		buffer[7] = 0;
	}
}

static uint8_t *
encode_integer_array(sproto_callback cb, struct sproto_arg *args, uint8_t *buffer, int size, int *noarray) {
	uint8_t * header = buffer;
	int intlen;
	int index;
	if (size < 1)
		return NULL;
	buffer++;
	size--;
	intlen = SIZEOF_INT32;
	index = 1;
	*noarray = 0;

	for (;;) {
		int sz;
		union {
			uint64_t u64;
			uint32_t u32;
		} u;
		args->value = &u;
		args->length = sizeof(u);
		args->index = index;
		sz = cb(args);
		if (sz <= 0) {
			if (sz == SPROTO_CB_NIL) // nil object, end of array
				break;
			if (sz == SPROTO_CB_NOARRAY) {	// no array, don't encode it
				*noarray = 1;
				break;
			}
			return NULL;	// sz == SPROTO_CB_ERROR
		}
		// notice: sizeof(uint64_t) is size_t (unsigned) , size may be negative. See issue #75
		// so use MACRO SIZOF_INT64 instead
		if (size < SIZEOF_INT64)
			return NULL;
		if (sz == SIZEOF_INT32) {
			uint32_t v = u.u32;
			buffer[0] = v & 0xff;
			buffer[1] = (v >> 8) & 0xff;
			buffer[2] = (v >> 16) & 0xff;
			buffer[3] = (v >> 24) & 0xff;

			if (intlen == SIZEOF_INT64) {
				uint32_to_uint64(v & 0x80000000, buffer);
			}
		}
		else {
			uint64_t v;
			if (sz != SIZEOF_INT64)
				return NULL;
			if (intlen == SIZEOF_INT32) {
				int i;
				// rearrange
				size -= (index - 1) * SIZEOF_INT32;
				if (size < SIZEOF_INT64)
					return NULL;
				buffer += (index - 1) * SIZEOF_INT32;
				for (i = index - 2; i >= 0; i--) {
					int negative;
					memcpy(header + 1 + i * SIZEOF_INT64, header + 1 + i * SIZEOF_INT32, SIZEOF_INT32);
					negative = header[1 + i * SIZEOF_INT64 + 3] & 0x80;
					uint32_to_uint64(negative, header + 1 + i * SIZEOF_INT64);
				}
				intlen = SIZEOF_INT64;
			}

			v = u.u64;
			buffer[0] = v & 0xff;
			buffer[1] = (v >> 8) & 0xff;
			buffer[2] = (v >> 16) & 0xff;
			buffer[3] = (v >> 24) & 0xff;
			buffer[4] = (v >> 32) & 0xff;
			buffer[5] = (v >> 40) & 0xff;
			buffer[6] = (v >> 48) & 0xff;
			buffer[7] = (v >> 56) & 0xff;
		}

		size -= intlen;
		buffer += intlen;
		index++;
	}

	if (buffer == header + 1) {
		return header;
	}
	*header = (uint8_t)intlen;
	return buffer;
}

static int
encode_array(sproto_callback cb, struct sproto_arg *args, uint8_t *data, int size) {
	uint8_t * buffer;
	int sz;
	if (size < SIZEOF_LENGTH)
		return -1;
	size -= SIZEOF_LENGTH;
	buffer = data + SIZEOF_LENGTH;
	switch (args->type) {
	case SPROTO_TINTEGER: {
		int noarray;
		buffer = encode_integer_array(cb, args, buffer, size, &noarray);
		if (buffer == NULL)
			return -1;

		if (noarray) {
			return 0;
		}
		break;
	}
	case SPROTO_TBOOLEAN:
		args->index = 1;
		for (;;) {
			int v = 0;
			args->value = &v;
			args->length = sizeof(v);
			sz = cb(args);
			if (sz < 0) {
				if (sz == SPROTO_CB_NIL)		// nil object , end of array
					break;
				if (sz == SPROTO_CB_NOARRAY)	// no array, don't encode it
					return 0;
				return -1;	// sz == SPROTO_CB_ERROR
			}
			if (size < 1)
				return -1;
			buffer[0] = v ? 1 : 0;
			size -= 1;
			buffer += 1;
			++args->index;
		}
		break;
	default:
		args->index = 1;
		for (;;) {
			if (size < SIZEOF_LENGTH)
				return -1;
			size -= SIZEOF_LENGTH;
			args->value = buffer + SIZEOF_LENGTH;
			args->length = size;
			sz = cb(args);
			if (sz < 0) {
				if (sz == SPROTO_CB_NIL) {
					break;
				}
				if (sz == SPROTO_CB_NOARRAY)	// no array, don't encode it
					return 0;
				return -1;	// sz == SPROTO_CB_ERROR
			}
			fill_size(buffer, sz);
			buffer += SIZEOF_LENGTH + sz;
			size -= sz;
			++args->index;
		}
		break;
	}
	sz = buffer - (data + SIZEOF_LENGTH);
	return fill_size(data, sz);
}

int
sproto_encode(const struct sproto_type *st, void * buffer, int size, sproto_callback cb, void *ud) {
	struct sproto_arg args;
	uint8_t * header = buffer;
	uint8_t * data;
	int header_sz = SIZEOF_HEADER + st->maxn * SIZEOF_FIELD;
	int i;
	int index;
	int lasttag;
	int datasz;
	if (size < header_sz)
		return -1;
	args.ud = ud;
	data = header + header_sz;
	size -= header_sz;
	index = 0;
	lasttag = -1;
	// fprintf(stderr, "sproto_encode st->name:%s st->n:%d\n", st->name, st->n);
	for (i = 0; i<st->n; i++) {
		struct sproto_field *f = &st->f[i];
		int type = f->type;
		int value = 0;
		int sz = -1;
		args.tagname = f->name;
		args.tagid = f->tag;
		args.subtype = f->st;
		args.mainindex = f->key;
		args.extra = f->extra;
#ifdef C_SPROTO
		args.c_struct_size = 0;
#endif
		if (type & SPROTO_TARRAY) {
			args.type = type & ~SPROTO_TARRAY;
#ifdef C_SPROTO
			if (SPROTO_TSTRUCT == args.type)
				args.c_struct_size = f->st->size;
#endif
			sz = encode_array(cb, &args, data, size);
		}
		else {
			args.type = type;
			args.index = 0;
			switch (type) {
			case SPROTO_TINTEGER:
			case SPROTO_TBOOLEAN: {
				union {
					uint64_t u64;
					uint32_t u32;
				} u;
				args.value = &u;
				args.length = sizeof(u);
				sz = cb(&args);
				if (sz < 0) {
					if (sz == SPROTO_CB_NIL)
						continue;
					if (sz == SPROTO_CB_NOARRAY)	// no array, don't encode it
						return 0;
					return -1;	// sz == SPROTO_CB_ERROR
				}
				if (sz == SIZEOF_INT32) {
					if (u.u32 < 0x7fff) {
						value = (u.u32 + 1) * 2;
						sz = 2; // sz can be any number > 0
					}
					else {
						sz = encode_integer(u.u32, data, size);
					}
				}
				else if (sz == SIZEOF_INT64) {
					sz = encode_uint64(u.u64, data, size);
				}
				else {
					return -1;
				}
				break;
			}
			case SPROTO_TSTRUCT:
			case SPROTO_TSTRING:
				sz = encode_object(cb, &args, data, size);
				break;
			}
		}
		if (sz < 0)
			return -1;
		if (sz > 0) {
			uint8_t * record;
			int tag;
			if (value == 0) {
				data += sz;
				size -= sz;
			}
			record = header + SIZEOF_HEADER + SIZEOF_FIELD * index;
			tag = f->tag - lasttag - 1;
			if (tag > 0) {
				// skip tag
				tag = (tag - 1) * 2 + 1;
				if (tag > 0xffff)
					return -1;
				record[0] = tag & 0xff;
				record[1] = (tag >> 8) & 0xff;
				++index;
				record += SIZEOF_FIELD;
			}
			++index;
			record[0] = value & 0xff;
			record[1] = (value >> 8) & 0xff;
			lasttag = f->tag;
		}
	}
	header[0] = index & 0xff;
	header[1] = (index >> 8) & 0xff;

	datasz = data - (header + header_sz);
	data = header + header_sz;
	if (index != st->maxn) {
		memmove(header + SIZEOF_HEADER + index * SIZEOF_FIELD, data, datasz);
	}
	return SIZEOF_HEADER + index * SIZEOF_FIELD + datasz;
}

static int
decode_array_object(sproto_callback cb, struct sproto_arg *args, uint8_t * stream, int sz) {
	uint32_t hsz;
	int index = 1;
	while (sz > 0) {
		if (sz < SIZEOF_LENGTH)
			return -1;
		hsz = todword(stream);
		stream += SIZEOF_LENGTH;
		sz -= SIZEOF_LENGTH;
		if (hsz > sz)
			return -1;
		args->index = index;
		args->value = stream;
		args->length = hsz;
		if (cb(args))
			return -1;
		sz -= hsz;
		stream += hsz;
		++index;
	}
	return 0;
}

static inline uint64_t
expand64(uint32_t v) {
	uint64_t value = v;
	if (value & 0x80000000) {
		value |= (uint64_t)~0 << 32;
	}
	return value;
}

static int
decode_array(sproto_callback cb, struct sproto_arg *args, uint8_t * stream) {
	uint32_t sz = todword(stream);
	int type = args->type;
	int i;
	if (sz == 0) {
		// It's empty array, call cb with index == -1 to create the empty array.
		args->index = -1;
		args->value = NULL;
		args->length = 0;
		cb(args);
		return 0;
	}
	stream += SIZEOF_LENGTH;
	switch (type) {
	case SPROTO_TINTEGER: {
		int len = *stream;
		++stream;
		--sz;
		if (len == SIZEOF_INT32) {
			if (sz % SIZEOF_INT32 != 0)
				return -1;
			for (i = 0; i<sz / SIZEOF_INT32; i++) {
				uint64_t value = expand64(todword(stream + i * SIZEOF_INT32));
				args->index = i + 1;
				args->value = &value;
				args->length = sizeof(value);
				cb(args);
			}
		}
		else if (len == SIZEOF_INT64) {
			if (sz % SIZEOF_INT64 != 0)
				return -1;
			for (i = 0; i<sz / SIZEOF_INT64; i++) {
				uint64_t low = todword(stream + i * SIZEOF_INT64);
				uint64_t hi = todword(stream + i * SIZEOF_INT64 + SIZEOF_INT32);
				uint64_t value = low | hi << 32;
				args->index = i + 1;
				args->value = &value;
				args->length = sizeof(value);
				cb(args);
			}
		}
		else {
			return -1;
		}
		break;
	}
	case SPROTO_TBOOLEAN:
		for (i = 0; i<sz; i++) {
			uint64_t value = stream[i];
			args->index = i + 1;
			args->value = &value;
			args->length = sizeof(value);
			cb(args);
		}
		break;
	case SPROTO_TSTRING:
	case SPROTO_TSTRUCT:
		return decode_array_object(cb, args, stream, sz);
	default:
		return -1;
	}
	return 0;
}

int
sproto_decode(const struct sproto_type *st, const void * data, int size, sproto_callback cb, void *ud) {
	struct sproto_arg args;
	int total = size;
	uint8_t * stream;
	uint8_t * datastream;
	int fn;
	int i;
	int tag;
	if (size < SIZEOF_HEADER)
		return -1;
	// debug print
	// printf("sproto_decode[%p] (%s)\n", ud, st->name);
	stream = (void *)data;
	fn = toword(stream);
	stream += SIZEOF_HEADER;
	size -= SIZEOF_HEADER;
	if (size < fn * SIZEOF_FIELD)
		return -1;
	datastream = stream + fn * SIZEOF_FIELD;
	size -= fn * SIZEOF_FIELD;
	args.ud = ud;

	tag = -1;
	for (i = 0; i<fn; i++) {
		uint8_t * currentdata;
		struct sproto_field * f;
		int value = toword(stream + i * SIZEOF_FIELD);
		++tag;
		if (value & 1) {
			tag += value / 2;
			continue;
		}
		value = value / 2 - 1;
		currentdata = datastream;
		if (value < 0) {
			uint32_t sz;
			if (size < SIZEOF_LENGTH)
				return -1;
			sz = todword(datastream);
			if (size < sz + SIZEOF_LENGTH)
				return -1;
			datastream += sz + SIZEOF_LENGTH;
			size -= sz + SIZEOF_LENGTH;
		}
		f = findtag(st, tag);
		if (f == NULL)
			continue;
		args.tagname = f->name;
		args.tagid = f->tag;
		args.type = f->type & ~SPROTO_TARRAY;
		args.subtype = f->st;
		args.index = 0;
		args.mainindex = f->key;
		args.extra = f->extra;
		if (value < 0) {
			if (f->type & SPROTO_TARRAY) {
				if (decode_array(cb, &args, currentdata)) {
					return -1;
				}
			}
			else {
				switch (f->type) {
				case SPROTO_TINTEGER: {
					uint32_t sz = todword(currentdata);
					if (sz == SIZEOF_INT32) {
						uint64_t v = expand64(todword(currentdata + SIZEOF_LENGTH));
						args.value = &v;
						args.length = sizeof(v);
						cb(&args);
					}
					else if (sz != SIZEOF_INT64) {
						return -1;
					}
					else {
						uint32_t low = todword(currentdata + SIZEOF_LENGTH);
						uint32_t hi = todword(currentdata + SIZEOF_LENGTH + SIZEOF_INT32);
						uint64_t v = (uint64_t)low | (uint64_t)hi << 32;
						args.value = &v;
						args.length = sizeof(v);
						cb(&args);
					}
					break;
				}
				case SPROTO_TSTRING:
				case SPROTO_TSTRUCT: {
					uint32_t sz = todword(currentdata);
					args.value = currentdata + SIZEOF_LENGTH;
					args.length = sz;
					if (cb(&args))
						return -1;
					break;
				}
				default:
					return -1;
				}
			}
		}
		else if (f->type != SPROTO_TINTEGER && f->type != SPROTO_TBOOLEAN) {
			return -1;
		}
		else {
			uint64_t v = value;
			args.value = &v;
			args.length = sizeof(v);
			cb(&args);
		}
	}
	return total - size;
}

// 0 pack

static int
pack_seg(const uint8_t *src, uint8_t * buffer, int sz, int n) {
	uint8_t header = 0;
	int notzero = 0;
	int i;
	uint8_t * obuffer = buffer;
	++buffer;
	--sz;
	if (sz < 0)
		obuffer = NULL;

	for (i = 0; i<8; i++) {
		if (src[i] != 0) {
			notzero++;
			header |= 1 << i;
			if (sz > 0) {
				*buffer = src[i];
				++buffer;
				--sz;
			}
		}
	}
	if ((notzero == 7 || notzero == 6) && n > 0) {
		notzero = 8;
	}
	if (notzero == 8) {
		if (n > 0) {
			return 8;
		}
		else {
			return 10;
		}
	}
	if (obuffer) {
		*obuffer = header;
	}
	return notzero + 1;
}

static inline void
write_ff(const uint8_t * src, uint8_t * des, int n) {
	int i;
	int align8_n = (n + 7)&(~7);

	des[0] = 0xff;
	des[1] = align8_n / 8 - 1;
	memcpy(des + 2, src, n);
	for (i = 0; i< align8_n - n; i++) {
		des[n + 2 + i] = 0;
	}
}

int
sproto_pack(const void * srcv, int srcsz, void * bufferv, int bufsz) {
	uint8_t tmp[8];
	int i;
	const uint8_t * ff_srcstart = NULL;
	uint8_t * ff_desstart = NULL;
	int ff_n = 0;
	int size = 0;
	const uint8_t * src = srcv;
	uint8_t * buffer = bufferv;
	for (i = 0; i<srcsz; i += 8) {
		int n;
		int padding = i + 8 - srcsz;
		if (padding > 0) {
			int j;
			memcpy(tmp, src, 8 - padding);
			for (j = 0; j<padding; j++) {
				tmp[7 - j] = 0;
			}
			src = tmp;
		}
		n = pack_seg(src, buffer, bufsz, ff_n);
		bufsz -= n;
		if (n == 10) {
			// first FF
			ff_srcstart = src;
			ff_desstart = buffer;
			ff_n = 1;
		}
		else if (n == 8 && ff_n>0) {
			++ff_n;
			if (ff_n == 256) {
				if (bufsz >= 0) {
					write_ff(ff_srcstart, ff_desstart, 256 * 8);
				}
				ff_n = 0;
			}
		}
		else {
			if (ff_n > 0) {
				if (bufsz >= 0) {
					write_ff(ff_srcstart, ff_desstart, ff_n * 8);
				}
				ff_n = 0;
			}
		}
		src += 8;
		buffer += n;
		size += n;
	}
	if (bufsz >= 0) {
		if (ff_n == 1)
			write_ff(ff_srcstart, ff_desstart, 8);
		else if (ff_n > 1)
			write_ff(ff_srcstart, ff_desstart, srcsz - (intptr_t)(ff_srcstart - (const uint8_t*)srcv));
	}
	return size;
}

int
sproto_unpack(const void * srcv, int srcsz, void * bufferv, int bufsz) {
	const uint8_t * src = srcv;
	uint8_t * buffer = bufferv;
	int size = 0;
	while (srcsz > 0) {
		uint8_t header = src[0];
		--srcsz;
		++src;
		if (header == 0xff) {
			int n;
			if (srcsz < 0) {
				return -1;
			}
			n = (src[0] + 1) * 8;
			if (srcsz < n + 1)
				return -1;
			srcsz -= n + 1;
			++src;
			if (bufsz >= n) {
				memcpy(buffer, src, n);
			}
			bufsz -= n;
			buffer += n;
			src += n;
			size += n;
		}
		else {
			int i;
			for (i = 0; i<8; i++) {
				int nz = (header >> i) & 1;
				if (nz) {
					if (srcsz < 0)
						return -1;
					if (bufsz > 0) {
						*buffer = *src;
						--bufsz;
						++buffer;
					}
					++src;
					--srcsz;
				}
				else {
					if (bufsz > 0) {
						*buffer = 0;
						--bufsz;
						++buffer;
					}
				}
				++size;
			}
		}
	}
	return size;
}
