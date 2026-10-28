/* Synthetic C API fixture; no Unity or game code/assets. Built freestanding. */
typedef __SIZE_TYPE__ usize;
typedef __UINTPTR_TYPE__ uptr;
typedef unsigned int u32;
typedef long long i64;
struct Type { int kind; const char *name; };
struct Class { int value; const char *name; struct Type *type; };
struct Object { struct Class *klass; void *monitor; union { i64 integer; double real; } value; };
struct String { struct Class *klass; void *monitor; int length; unsigned short chars[128]; };
struct Method { const char *name; unsigned count, flags; struct Type *result, *args[5]; };
static struct Type integer = {10,"System.Int64"}, real = {13,"System.Double"}, boolean = {2,"System.Boolean"}, str = {14,"System.String"}, objtype = {18,"Fixture.Probe"};
static struct Class probe = {0,"Probe",&objtype}, intclass = {1,"Int64",&integer}, realclass = {1,"Double",&real}, boolclass = {1,"Boolean",&boolean}, strclass = {0,"String",&str};
static struct Object instance = {&probe,0,{7}}, answer = {&intclass,0,{0}};
static struct String input = {&strclass,0,0,{0}};
static struct Method mix = {"Mix",4,16,&integer,{&integer,&real,&boolean,&str}}, bump = {"Bump",1,0,&integer,{&integer}}, get = {"Get",0,16,&objtype,{0}};
static int facade, distinct_facade;
static struct Class alternate_probe = {0,"Probe",&objtype};
static struct Class base_probe = {0,"BaseProbe",&objtype};
static struct Method base_only = {"BaseOnly",0,0,&integer,{0}};
static struct Type type_type = {18,"System.Type"}, array_type = {29,"UnityEngine.Object[]"};
static struct Class type_class = {0,"Type",&type_type}, array_class = {0,"Object[]",&array_type};
static struct Class object_class = {0,"Object",&objtype}, resources_class = {0,"Resources",&objtype};
static struct Type sort_type = {8,"UnityEngine.FindObjectsSortMode"}, scene_type = {17,"UnityEngine.SceneManagement.Scene"};
static struct Class sort_class = {1,"FindObjectsSortMode",&sort_type}, scene_class = {1,"Scene",&scene_type};
static struct Class component_class = {0,"Component",&objtype}, gameobject_class = {0,"GameObject",&objtype};
static struct Object scene_go = {&gameobject_class,0,{1}}, asset_go = {&gameobject_class,0,{0}}, inactive_go = {&gameobject_class,0,{2}}, scene_value = {&scene_class,0,{0}}, bool_value = {&boolclass,0,{0}};
static struct Method get_name = {"get_name",0,0,&str,{0}};
static struct Method find_modern = {"FindObjectsByType",2,16,&array_type,{&type_type,&sort_type}}, get_go = {"get_gameObject",0,0,&objtype,{0}}, get_scene = {"get_scene",0,0,&scene_type,{0}}, is_valid = {"IsValid",0,0,&boolean,{0}}, is_active = {"get_activeInHierarchy",0,0,&boolean,{0}};
static int lookup_mode, lookup_used, search_failure, search_calls;
void fixture_search_failure(int mode) { search_failure=mode; search_calls=0; }
int fixture_search_calls(void) { return search_calls; }
void fixture_lookup_mode(int mode) { lookup_mode=mode; lookup_used=0; }
int fixture_lookup_used(void) { return lookup_used; }
static struct Object type_object = {&type_class,0,{0}}, asset = {&probe,0,{99}}, inactive = {&probe,0,{101}};
static struct Method find_active = {"FindObjectsOfType",1,16,&array_type,{&type_type}}, find_all = {"FindObjectsOfTypeAll",1,16,&array_type,{&type_type}};
static struct { struct Class *klass; void *monitor, *bounds; usize length; struct Object *items[3]; } found_objects = {&array_class,0,0,0,{&instance,&asset,&inactive}};
static void *assemblies[2] = {&probe,&facade};
static void **assembly_storage = assemblies;
void **fixture_assembly_storage(void) { return assembly_storage; }
void fixture_set_assembly_storage(void **p) { assembly_storage=p; }
void fixture_distinct_facade(int distinct) { distinct_facade=distinct; }
static struct Object *roots[4096];
static u32 next_root = 1;
u32 fixture_retained_handles(void) {
  u32 count=0;
  for (u32 i=1;i<next_root;++i) if (roots[i]) ++count;
  return count;
}
void il2cpp_format_exception(void *exception,char *buffer,int length) {
  (void)exception;
  const char *message="fixture search property failure";
  int i=0;
  while (i+1<length && message[i]) { buffer[i]=message[i]; ++i; }
  if (length>0) buffer[i]=0;
}
static int equal(const char *a,const char *b) { while (*a && *a == *b) { ++a; ++b; } return *a == *b; }
void *il2cpp_domain_get(void) { return &probe; }
void **il2cpp_domain_get_assemblies(void *domain,usize *count) { *count=domain?2:0; return assembly_storage; }
void *il2cpp_assembly_get_image(void *assembly) { return assembly; }
const char *il2cpp_image_get_name(void *image) { return image==&facade?"Forwarder.dll":"Fixture.dll"; }
usize il2cpp_image_get_class_count(void *image) { return image==&facade?0:1; }
void *il2cpp_image_get_class(void *image,usize n) { (void)image; return n?0:&probe; }
void *il2cpp_class_from_name(void *image,const char *ns,const char *name) {
  if(equal(ns,"UnityEngine")) return equal(name,"Object")?&object_class:equal(name,"Resources")?&resources_class:equal(name,"Component")?&component_class:equal(name,"GameObject")?&gameobject_class:0;
  if(equal(ns,"UnityEngine.SceneManagement")&&equal(name,"Scene")) return &scene_class;
  return equal(ns,"Fixture")&&equal(name,"Probe")?(image==&facade&&distinct_facade?&alternate_probe:&probe):0;
}
const char *il2cpp_class_get_name(struct Class *c) { return c->name; }
const char *il2cpp_class_get_namespace(struct Class *c) { (void)c; return "Fixture"; }
void *il2cpp_class_get_type(struct Class *c) { return c->type; }
void *il2cpp_class_get_parent(struct Class *c) { return c==&probe?&base_probe:c==&base_probe?&component_class:c==&component_class||c==&gameobject_class?&object_class:0; }
void *il2cpp_class_from_type(struct Type *t) { return t==&integer?&intclass:t==&real?&realclass:t==&boolean?&boolclass:t==&str?&strclass:t==&type_type?&type_class:t==&array_type?&array_class:t==&sort_type?&sort_class:t==&scene_type?&scene_class:&probe; }
int il2cpp_class_is_valuetype(struct Class *c) { return c->value; }
int il2cpp_class_is_enum(struct Class *c) { (void)c; return 0; }
int il2cpp_class_is_assignable_from(struct Class *a,struct Class *b) { while(b) { if(a==b)return 1; b=il2cpp_class_get_parent(b); } return 0; }
void *il2cpp_class_get_methods(void *c,void **iter) {
  uptr i=(uptr)*iter; *iter=(void *)(i+1);
  if(c==&base_probe) return i==0?&base_only:0;
  if(c==&object_class) return i==0?&get_name:i==1?(lookup_mode==0?&find_active:lookup_mode==1?&find_modern:0):0;
  if(c==&resources_class) return i==0?&find_all:0;
  if(c==&component_class) return i==0?&get_go:0;
  if(c==&gameobject_class) return i==0?&get_scene:i==1?&is_active:0;
  if(c==&scene_class) return i==0?&is_valid:0;
  return i==0?&mix:i==1?&bump:i==2?&get:0;
}
const char *il2cpp_method_get_name(struct Method *m) { return m->name; }
unsigned il2cpp_method_get_param_count(struct Method *m) { return m->count; }
void *il2cpp_method_get_param(struct Method *m,unsigned n) { return m->args[n]; }
void *il2cpp_method_get_return_type(struct Method *m) { return m->result; }
unsigned il2cpp_method_get_flags(struct Method *m,unsigned *impl) { *impl=0; return m->flags; }
int il2cpp_type_get_type(struct Type *t) { return t->kind; }
int il2cpp_type_is_byref(struct Type *t) { (void)t; return 0; }
void *il2cpp_type_get_object(struct Type *t) { (void)t; return &type_object; }
usize il2cpp_array_length(void *p) { return p==&found_objects?found_objects.length:0; }
const char *il2cpp_type_get_name(struct Type *t) { return t->name; }
void il2cpp_free(void *p) { (void)p; }
void *il2cpp_object_get_class(struct Object *o) { return o->klass; }
void *il2cpp_object_unbox(struct Object *o) { return &o->value; }
u32 il2cpp_gchandle_new(struct Object *p,int pinned) { (void)pinned; u32 h=next_root++; roots[h]=p; return h; }
void *il2cpp_gchandle_get_target(u32 h) { return roots[h]; }
void il2cpp_gchandle_free(u32 h) { roots[h]=0; }
void *il2cpp_string_new(const char *s) { input.length=0; while (*s && input.length<127) input.chars[input.length++]=(unsigned char)*s++; return &input; }
int il2cpp_string_length(struct String *s) { return s->length; }
void *il2cpp_string_chars(struct String *s) { return s->chars; }
void *il2cpp_runtime_invoke(struct Method *m,struct Object *self,void **args,void **exception) {
  *exception=0;
  if ((search_failure==1 && m==&get_name) || (search_failure==2 && m==&get_scene)) {
    if (++search_calls==2) { *exception=&instance; return 0; }
  }
  if (m==&get) return &instance;
  if (m==&base_only) { answer.value.integer=self->value.integer; return &answer; }
  if (m==&get_name) return il2cpp_string_new(self==&asset||self==&inactive?"scene-inactive":"scene-active");
  if (m==&get_go) return self==&asset?&asset_go:self==&inactive?&inactive_go:&scene_go;
  if (m==&get_scene) { scene_value.value.integer=self->value.integer; return &scene_value; }
  if (m==&is_valid) { bool_value.value.integer=*(i64 *)self!=0; return &bool_value; }
  if (m==&is_active) { bool_value.value.integer=self!=&inactive_go; return &bool_value; }
  if (m==&find_active || m==&find_all || m==&find_modern) {
    lookup_used=m==&find_active?1:m==&find_modern?2:3;
    if(m==&find_modern && *(int *)args[1]!=0) { *exception=&instance; return 0; }
    found_objects.length=args[0]==&type_object?(m==&find_all?3:1):0;
    return &found_objects;
  }
  if (m==&bump) { self->value.integer+=*(i64 *)args[0]; answer.value.integer=self->value.integer; return &answer; }
  struct String *s=(struct String *)args[3];
  unsigned long long bits=*(unsigned long long *)args[1];
  answer.value.integer=*(i64 *)args[0]+(bits==0x4029000000000000ull?12:bits==0xc004000000000000ull?-2:-1000)+(*(unsigned char *)args[2]?100:0)+s->length;
  return &answer;
}
static int ref_field, static_ref_field, text_field;
static struct Object *reference, *static_reference;
static struct String *text_reference;
int fixture_reference_is_instance(int is_static) { return (is_static?static_reference:reference)==&instance; }
int fixture_reference_is_null(int is_static) { return (is_static?static_reference:reference)==0; }
int fixture_text_is_input(void) { return text_reference==&input; }
void *il2cpp_class_get_field_from_name(void *c,const char *name) { (void)c; return equal(name,"value")?&integer:equal(name,"reference")?&ref_field:equal(name,"staticReference")?&static_ref_field:equal(name,"text")?&text_field:0; }
const char *il2cpp_field_get_name(void *f) { (void)f; return "value"; }
void *il2cpp_field_get_type(void *f) { return f==&ref_field||f==&static_ref_field?&objtype:f==&text_field?&str:f; }
unsigned il2cpp_field_get_flags(void *f) { return f==&static_ref_field?16:0; }
void *il2cpp_field_get_value_object(void *f,struct Object *o) { if(f==&ref_field)return reference; if(f==&static_ref_field)return static_reference; if(f==&text_field)return text_reference; answer.value.integer=o->value.integer; return &answer; }
void il2cpp_field_set_value(struct Object *o,void *f,void *value) { if(f==&ref_field)reference=value; else if(f==&text_field)text_reference=value; else o->value.integer=*(i64 *)value; }
void il2cpp_field_static_set_value(void *f,void *value) { if(f==&static_ref_field)static_reference=value; }
void *il2cpp_class_get_fields(void *c,void **iter) { (void)c; if (*iter) return 0; *iter=(void *)1; return &integer; }
