/*
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 *
 * based in part on anv driver which is:
 * Copyright © 2015 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <stdlib.h>
#include <stdio.h>
#include <sys/utsname.h>

#include "util/mesa-sha1.h"
#include "sid.h"
#include "ac_debug.h"
#include "radv_debug.h"
#include "radv_shader.h"

#define TRACE_BO_SIZE 4096
#define TMA_BO_SIZE 4096

#define COLOR_RESET	"\033[0m"
#define COLOR_RED	"\033[31m"
#define COLOR_GREEN	"\033[1;32m"
#define COLOR_YELLOW	"\033[1;33m"
#define COLOR_CYAN	"\033[1;36m"

/* Trace BO layout (offsets are 4 bytes):
 *
 * [0]: primary trace ID
 * [1]: secondary trace ID
 * [2-3]: 64-bit GFX ring pipeline pointer
 * [4-5]: 64-bit COMPUTE ring pipeline pointer
 * [6-7]: 64-bit descriptor set #0 pointer
 * ...
 * [68-69]: 64-bit descriptor set #31 pointer
 */

bool
radv_init_trace(struct radv_device *device)
{
	struct radeon_winsys *ws = device->ws;

	device->trace_bo = ws->buffer_create(ws, TRACE_BO_SIZE, 8,
					     RADEON_DOMAIN_VRAM,
					     RADEON_FLAG_CPU_ACCESS|
					     RADEON_FLAG_NO_INTERPROCESS_SHARING |
					     RADEON_FLAG_ZERO_VRAM,
					     RADV_BO_PRIORITY_UPLOAD_BUFFER);
	if (!device->trace_bo)
		return false;

	device->trace_id_ptr = ws->buffer_map(device->trace_bo);
	if (!device->trace_id_ptr)
		return false;

	ac_vm_fault_occured(device->physical_device->rad_info.chip_class,
			    &device->dmesg_timestamp, NULL);

	return true;
}

static void
radv_dump_trace(struct radv_device *device, struct radeon_cmdbuf *cs)
{
	const char *filename = getenv("RADV_TRACE_FILE");
	FILE *f = fopen(filename, "w");

	if (!f) {
		fprintf(stderr, "Failed to write trace dump to %s\n", filename);
		return;
	}

	fprintf(f, "Trace ID: %x\n", *device->trace_id_ptr);
	device->ws->cs_dump(cs, f, (const int*)device->trace_id_ptr, 2);
	fclose(f);
}

static void
radv_dump_mmapped_reg(struct radv_device *device, FILE *f, unsigned offset)
{
	struct radeon_winsys *ws = device->ws;
	uint32_t value;

	if (ws->read_registers(ws, offset, 1, &value))
		ac_dump_reg(f, device->physical_device->rad_info.chip_class,
			    offset, value, ~0);
}

static void
radv_dump_debug_registers(struct radv_device *device, FILE *f)
{
	struct radeon_info *info = &device->physical_device->rad_info;

	fprintf(f, "Memory-mapped registers:\n");
	radv_dump_mmapped_reg(device, f, R_008010_GRBM_STATUS);

	/* No other registers can be read on DRM < 3.1.0. */
	if (info->drm_minor < 1) {
		fprintf(f, "\n");
		return;
	}

	radv_dump_mmapped_reg(device, f, R_008008_GRBM_STATUS2);
	radv_dump_mmapped_reg(device, f, R_008014_GRBM_STATUS_SE0);
	radv_dump_mmapped_reg(device, f, R_008018_GRBM_STATUS_SE1);
	radv_dump_mmapped_reg(device, f, R_008038_GRBM_STATUS_SE2);
	radv_dump_mmapped_reg(device, f, R_00803C_GRBM_STATUS_SE3);
	radv_dump_mmapped_reg(device, f, R_00D034_SDMA0_STATUS_REG);
	radv_dump_mmapped_reg(device, f, R_00D834_SDMA1_STATUS_REG);
	if (info->chip_class <= GFX8) {
		radv_dump_mmapped_reg(device, f, R_000E50_SRBM_STATUS);
		radv_dump_mmapped_reg(device, f, R_000E4C_SRBM_STATUS2);
		radv_dump_mmapped_reg(device, f, R_000E54_SRBM_STATUS3);
	}
	radv_dump_mmapped_reg(device, f, R_008680_CP_STAT);
	radv_dump_mmapped_reg(device, f, R_008674_CP_STALLED_STAT1);
	radv_dump_mmapped_reg(device, f, R_008678_CP_STALLED_STAT2);
	radv_dump_mmapped_reg(device, f, R_008670_CP_STALLED_STAT3);
	radv_dump_mmapped_reg(device, f, R_008210_CP_CPC_STATUS);
	radv_dump_mmapped_reg(device, f, R_008214_CP_CPC_BUSY_STAT);
	radv_dump_mmapped_reg(device, f, R_008218_CP_CPC_STALLED_STAT1);
	radv_dump_mmapped_reg(device, f, R_00821C_CP_CPF_STATUS);
	radv_dump_mmapped_reg(device, f, R_008220_CP_CPF_BUSY_STAT);
	radv_dump_mmapped_reg(device, f, R_008224_CP_CPF_STALLED_STAT1);
	fprintf(f, "\n");
}

static void
radv_dump_buffer_descriptor(enum chip_class chip_class, const uint32_t *desc,
			    FILE *f)
{
	fprintf(f, COLOR_CYAN "    Buffer:" COLOR_RESET "\n");
	for (unsigned j = 0; j < 4; j++)
		ac_dump_reg(f, chip_class, R_008F00_SQ_BUF_RSRC_WORD0 + j * 4,
			    desc[j], 0xffffffff);
}

static void
radv_dump_image_descriptor(enum chip_class chip_class, const uint32_t *desc,
			   FILE *f)
{
	unsigned sq_img_rsrc_word0 = chip_class >= GFX10 ? R_00A000_SQ_IMG_RSRC_WORD0
							 : R_008F10_SQ_IMG_RSRC_WORD0;

	fprintf(f, COLOR_CYAN "    Image:" COLOR_RESET "\n");
	for (unsigned j = 0; j < 8; j++)
		ac_dump_reg(f, chip_class, sq_img_rsrc_word0 + j * 4,
			    desc[j], 0xffffffff);

	fprintf(f, COLOR_CYAN "    FMASK:" COLOR_RESET "\n");
	for (unsigned j = 0; j < 8; j++)
		ac_dump_reg(f, chip_class, sq_img_rsrc_word0 + j * 4,
			    desc[8 + j], 0xffffffff);
}

static void
radv_dump_sampler_descriptor(enum chip_class chip_class, const uint32_t *desc,
			     FILE *f)
{
	fprintf(f, COLOR_CYAN "    Sampler state:" COLOR_RESET "\n");
	for (unsigned j = 0; j < 4; j++) {
		ac_dump_reg(f, chip_class, R_008F30_SQ_IMG_SAMP_WORD0 + j * 4,
			    desc[j], 0xffffffff);
	}
}

static void
radv_dump_combined_image_sampler_descriptor(enum chip_class chip_class,
					    const uint32_t *desc, FILE *f)
{
	radv_dump_image_descriptor(chip_class, desc, f);
	radv_dump_sampler_descriptor(chip_class, desc + 16, f);
}

static void
radv_dump_descriptor_set(struct radv_device *device,
			 struct radv_descriptor_set *set, unsigned id, FILE *f)
{
	enum chip_class chip_class = device->physical_device->rad_info.chip_class;
	const struct radv_descriptor_set_layout *layout;
	int i;

	if (!set)
		return;
	layout = set->layout;

	for (i = 0; i < set->layout->binding_count; i++) {
		uint32_t *desc =
			set->mapped_ptr + layout->binding[i].offset / 4;

		switch (layout->binding[i].type) {
		case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
		case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
		case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
		case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
			radv_dump_buffer_descriptor(chip_class, desc, f);
			break;
		case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
		case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
		case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
			radv_dump_image_descriptor(chip_class, desc, f);
			break;
		case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
			radv_dump_combined_image_sampler_descriptor(chip_class, desc, f);
			break;
		case VK_DESCRIPTOR_TYPE_SAMPLER:
			radv_dump_sampler_descriptor(chip_class, desc, f);
			break;
		case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
		case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
			/* todo */
			break;
		default:
			assert(!"unknown descriptor type");
			break;
		}
		fprintf(f, "\n");
	}
	fprintf(f, "\n\n");
}

static void
radv_dump_descriptors(struct radv_device *device, FILE *f)
{
	uint64_t *ptr = (uint64_t *)device->trace_id_ptr;
	int i;

	fprintf(f, "Descriptors:\n");
	for (i = 0; i < MAX_SETS; i++) {
		struct radv_descriptor_set *set =
			*(struct radv_descriptor_set **)(ptr + i + 3);

		radv_dump_descriptor_set(device, set, i, f);
	}
}

struct radv_shader_inst {
	char text[160];  /* one disasm line */
	unsigned offset; /* instruction offset */
	unsigned size;   /* instruction size = 4 or 8 */
};

/* Split a disassembly string into lines and add them to the array pointed
 * to by "instructions". */
static void si_add_split_disasm(const char *disasm,
				uint64_t start_addr,
				unsigned *num,
				struct radv_shader_inst *instructions)
{
	struct radv_shader_inst *last_inst = *num ? &instructions[*num - 1] : NULL;
	char *next;

	while ((next = strchr(disasm, '\n'))) {
		struct radv_shader_inst *inst = &instructions[*num];
		unsigned len = next - disasm;

		if (!memchr(disasm, ';', len)) {
			/* Ignore everything that is not an instruction. */
			disasm = next + 1;
			continue;
		}

		assert(len < ARRAY_SIZE(inst->text));
		memcpy(inst->text, disasm, len);
		inst->text[len] = 0;
		inst->offset = last_inst ? last_inst->offset + last_inst->size : 0;

		const char *semicolon = strchr(disasm, ';');
		assert(semicolon);
		/* More than 16 chars after ";" means the instruction is 8 bytes long. */
		inst->size = next - semicolon > 16 ? 8 : 4;

		snprintf(inst->text + len, ARRAY_SIZE(inst->text) - len,
			" [PC=0x%"PRIx64", off=%u, size=%u]",
			start_addr + inst->offset, inst->offset, inst->size);

		last_inst = inst;
		(*num)++;
		disasm = next + 1;
	}
}

static void
radv_dump_annotated_shader(struct radv_shader_variant *shader,
			   gl_shader_stage stage, struct ac_wave_info *waves,
			   unsigned num_waves, FILE *f)
{
	uint64_t start_addr, end_addr;
	unsigned i;

	if (!shader)
		return;

	start_addr = radv_buffer_get_va(shader->bo) + shader->bo_offset;
	end_addr = start_addr + shader->code_size;

	/* See if any wave executes the shader. */
	for (i = 0; i < num_waves; i++) {
		if (start_addr <= waves[i].pc && waves[i].pc <= end_addr)
			break;
	}

	if (i == num_waves)
		return; /* the shader is not being executed */

	/* Remember the first found wave. The waves are sorted according to PC. */
	waves = &waves[i];
	num_waves -= i;

	/* Get the list of instructions.
	 * Buffer size / 4 is the upper bound of the instruction count.
	 */
	unsigned num_inst = 0;
	struct radv_shader_inst *instructions =
		calloc(shader->code_size / 4, sizeof(struct radv_shader_inst));

	si_add_split_disasm(shader->disasm_string,
			    start_addr, &num_inst, instructions);

	fprintf(f, COLOR_YELLOW "%s - annotated disassembly:" COLOR_RESET "\n",
		radv_get_shader_name(&shader->info, stage));

	/* Print instructions with annotations. */
	for (i = 0; i < num_inst; i++) {
		struct radv_shader_inst *inst = &instructions[i];

		fprintf(f, "%s\n", inst->text);

		/* Print which waves execute the instruction right now. */
		while (num_waves && start_addr + inst->offset == waves->pc) {
			fprintf(f,
				"          " COLOR_GREEN "^ SE%u SH%u CU%u "
				"SIMD%u WAVE%u  EXEC=%016"PRIx64 "  ",
				waves->se, waves->sh, waves->cu, waves->simd,
				waves->wave, waves->exec);

			if (inst->size == 4) {
				fprintf(f, "INST32=%08X" COLOR_RESET "\n",
					waves->inst_dw0);
			} else {
				fprintf(f, "INST64=%08X %08X" COLOR_RESET "\n",
					waves->inst_dw0, waves->inst_dw1);
			}

			waves->matched = true;
			waves = &waves[1];
			num_waves--;
		}
	}

	fprintf(f, "\n\n");
	free(instructions);
}

static void
radv_dump_annotated_shaders(struct radv_pipeline *pipeline,
			    VkShaderStageFlagBits active_stages, FILE *f)
{
	struct ac_wave_info waves[AC_MAX_WAVES_PER_CHIP];
	enum chip_class chip_class = pipeline->device->physical_device->rad_info.chip_class;
	unsigned num_waves = ac_get_wave_info(chip_class, waves);

	fprintf(f, COLOR_CYAN "The number of active waves = %u" COLOR_RESET
		"\n\n", num_waves);

	/* Dump annotated active graphics shaders. */
	while (active_stages) {
		int stage = u_bit_scan(&active_stages);

		radv_dump_annotated_shader(pipeline->shaders[stage],
					   stage, waves, num_waves, f);
	}

	/* Print waves executing shaders that are not currently bound. */
	unsigned i;
	bool found = false;
	for (i = 0; i < num_waves; i++) {
		if (waves[i].matched)
			continue;

		if (!found) {
			fprintf(f, COLOR_CYAN
				"Waves not executing currently-bound shaders:"
				COLOR_RESET "\n");
			found = true;
		}
		fprintf(f, "    SE%u SH%u CU%u SIMD%u WAVE%u  EXEC=%016"PRIx64
			"  INST=%08X %08X  PC=%"PRIx64"\n",
			waves[i].se, waves[i].sh, waves[i].cu, waves[i].simd,
			waves[i].wave, waves[i].exec, waves[i].inst_dw0,
			waves[i].inst_dw1, waves[i].pc);
	}
	if (found)
		fprintf(f, "\n\n");
}

static void
radv_dump_shader(struct radv_pipeline *pipeline,
		 struct radv_shader_variant *shader, gl_shader_stage stage,
		 FILE *f)
{
	if (!shader)
		return;

	fprintf(f, "%s:\n\n", radv_get_shader_name(&shader->info, stage));

	if (shader->spirv) {
		unsigned char sha1[21];
		char sha1buf[41];

		_mesa_sha1_compute(shader->spirv, shader->spirv_size, sha1);
		_mesa_sha1_format(sha1buf, sha1);

		fprintf(f, "SPIRV (sha1: %s):\n", sha1buf);
		radv_print_spirv(shader->spirv, shader->spirv_size, f);
	}

	if (shader->nir_string) {
		fprintf(f, "NIR:\n%s\n", shader->nir_string);
	}

	fprintf(f, "%s IR:\n%s\n",
		pipeline->device->physical_device->use_llvm ? "LLVM" : "ACO",
		shader->ir_string);
	fprintf(f, "DISASM:\n%s\n", shader->disasm_string);

	radv_dump_shader_stats(pipeline->device, pipeline, stage, f);
}

static void
radv_dump_shaders(struct radv_pipeline *pipeline,
		  VkShaderStageFlagBits active_stages, FILE *f)
{
	/* Dump active graphics shaders. */
	while (active_stages) {
		int stage = u_bit_scan(&active_stages);

		radv_dump_shader(pipeline, pipeline->shaders[stage], stage, f);
	}
}

static struct radv_pipeline *
radv_get_saved_pipeline(struct radv_device *device, enum ring_type ring)
{
	uint64_t *ptr = (uint64_t *)device->trace_id_ptr;
	int offset = ring == RING_GFX ? 1 : 2;

	return *(struct radv_pipeline **)(ptr + offset);
}

static void
radv_dump_queue_state(struct radv_queue *queue, FILE *f)
{
	enum ring_type ring = radv_queue_family_to_ring(queue->queue_family_index);
	struct radv_pipeline *pipeline;

	fprintf(f, "RING_%s:\n", ring == RING_GFX ? "GFX" : "COMPUTE");

	pipeline = radv_get_saved_pipeline(queue->device, ring);
	if (pipeline) {
		radv_dump_shaders(pipeline, pipeline->active_stages, f);
		radv_dump_annotated_shaders(pipeline, pipeline->active_stages, f);
		radv_dump_descriptors(queue->device, f);
	}
}

static void
radv_dump_dmesg(FILE *f)
{
	char line[2000];
	FILE *p;

	p = popen("dmesg | tail -n60", "r");
	if (!p)
		return;

	fprintf(f, "\nLast 60 lines of dmesg:\n\n");
	while (fgets(line, sizeof(line), p))
		fputs(line, f);
	fprintf(f, "\n");

	pclose(p);
}

void
radv_dump_enabled_options(struct radv_device *device, FILE *f)
{
	uint64_t mask;

	if (device->instance->debug_flags) {
		fprintf(f, "Enabled debug options: ");

		mask = device->instance->debug_flags;
		while (mask) {
			int i = u_bit_scan64(&mask);
			fprintf(f, "%s, ", radv_get_debug_option_name(i));
		}
		fprintf(f, "\n");
	}

	if (device->instance->perftest_flags) {
		fprintf(f, "Enabled perftest options: ");

		mask = device->instance->perftest_flags;
		while (mask) {
			int i = u_bit_scan64(&mask);
			fprintf(f, "%s, ", radv_get_perftest_option_name(i));
		}
		fprintf(f, "\n");
	}
}

static void
radv_dump_device_name(struct radv_device *device, FILE *f)
{
	struct radeon_info *info = &device->physical_device->rad_info;
	char kernel_version[128] = {};
	struct utsname uname_data;
	const char *chip_name;

	chip_name = device->ws->get_chip_name(device->ws);

	if (uname(&uname_data) == 0)
		snprintf(kernel_version, sizeof(kernel_version),
			 " / %s", uname_data.release);

	fprintf(f, "Device name: %s (%s / DRM %i.%i.%i%s)\n\n",
		chip_name, device->physical_device->name,
		info->drm_major, info->drm_minor, info->drm_patchlevel,
		kernel_version);
}

static bool
radv_gpu_hang_occured(struct radv_queue *queue, enum ring_type ring)
{
	struct radeon_winsys *ws = queue->device->ws;

	if (!ws->ctx_wait_idle(queue->hw_ctx, ring, queue->queue_idx))
		return true;

	return false;
}

void
radv_check_gpu_hangs(struct radv_queue *queue, struct radeon_cmdbuf *cs)
{
	struct radv_device *device = queue->device;
	enum ring_type ring;
	uint64_t addr;

	ring = radv_queue_family_to_ring(queue->queue_family_index);

	bool hang_occurred = radv_gpu_hang_occured(queue, ring);
	bool vm_fault_occurred = false;
	if (queue->device->instance->debug_flags & RADV_DEBUG_VM_FAULTS)
		vm_fault_occurred = ac_vm_fault_occured(device->physical_device->rad_info.chip_class,
		                                        &device->dmesg_timestamp, &addr);
	if (!hang_occurred && !vm_fault_occurred)
		return;

	radv_dump_trace(queue->device, cs);

	fprintf(stderr, "GPU hang report:\n\n");
	radv_dump_device_name(device, stderr);
	ac_print_gpu_info(&device->physical_device->rad_info);

	radv_dump_enabled_options(device, stderr);
	radv_dump_dmesg(stderr);

	if (vm_fault_occurred) {
		fprintf(stderr, "VM fault report.\n\n");
		fprintf(stderr, "Failing VM page: 0x%08"PRIx64"\n\n", addr);
	}

	radv_dump_debug_registers(device, stderr);
	radv_dump_queue_state(queue, stderr);

	abort();
}

void
radv_print_spirv(const char *data, uint32_t size, FILE *fp)
{
	char path[] = "/tmp/fileXXXXXX";
	char line[2048], command[128];
	FILE *p;
	int fd;

	/* Dump the binary into a temporary file. */
	fd = mkstemp(path);
	if (fd < 0)
		return;

	if (write(fd, data, size) == -1)
		goto fail;

	sprintf(command, "spirv-dis %s", path);

	/* Disassemble using spirv-dis if installed. */
	p = popen(command, "r");
	if (p) {
		while (fgets(line, sizeof(line), p))
			fprintf(fp, "%s", line);
		pclose(p);
	}

fail:
	close(fd);
	unlink(path);
}

bool
radv_trap_handler_init(struct radv_device *device)
{
	struct radeon_winsys *ws = device->ws;

	/* Create the trap handler shader and upload it like other shaders. */
	device->trap_handler_shader = radv_create_trap_handler_shader(device);
	if (!device->trap_handler_shader) {
		fprintf(stderr, "radv: failed to create the trap handler shader.\n");
		return false;
	}

	device->tma_bo = ws->buffer_create(ws, TMA_BO_SIZE, 256,
					   RADEON_DOMAIN_VRAM,
					   RADEON_FLAG_CPU_ACCESS |
					   RADEON_FLAG_NO_INTERPROCESS_SHARING |
					   RADEON_FLAG_ZERO_VRAM |
					   RADEON_FLAG_32BIT,
					   RADV_BO_PRIORITY_SCRATCH);
	if (!device->tma_bo)
		return false;

	device->tma_ptr = ws->buffer_map(device->tma_bo);
	if (!device->tma_ptr)
		return false;

	/* Upload a buffer descriptor to store various info from the trap. */
	uint64_t tma_va = radv_buffer_get_va(device->tma_bo) + 16;
	uint32_t desc[4];

	desc[0] = tma_va;
	desc[1] = S_008F04_BASE_ADDRESS_HI(tma_va >> 32);
	desc[2] = TMA_BO_SIZE;
	desc[3] = S_008F0C_DST_SEL_X(V_008F0C_SQ_SEL_X) |
		  S_008F0C_DST_SEL_Y(V_008F0C_SQ_SEL_Y) |
		  S_008F0C_DST_SEL_Z(V_008F0C_SQ_SEL_Z) |
		  S_008F0C_DST_SEL_W(V_008F0C_SQ_SEL_W) |
		  S_008F0C_DATA_FORMAT(V_008F0C_BUF_DATA_FORMAT_32);

	memcpy(device->tma_ptr, desc, sizeof(desc));

	return true;
}

void
radv_trap_handler_finish(struct radv_device *device)
{
	struct radeon_winsys *ws = device->ws;

	if (unlikely(device->trap_handler_shader))
		radv_shader_variant_destroy(device, device->trap_handler_shader);

	if (unlikely(device->tma_bo))
		ws->buffer_destroy(device->tma_bo);
}

static struct radv_shader_variant *
radv_get_faulty_shader(struct radv_device *device, uint64_t faulty_pc)
{
	struct radv_shader_variant *shader = NULL;

	mtx_lock(&device->shader_slab_mutex);
	list_for_each_entry(struct radv_shader_slab, slab, &device->shader_slabs, slabs) {
		list_for_each_entry(struct radv_shader_variant, s, &slab->shaders, slab_list) {
			uint64_t offset = align_u64(s->bo_offset + s->code_size, 256);
			uint64_t va = radv_buffer_get_va(s->bo);

			if (faulty_pc >= va + s->bo_offset && faulty_pc < va + offset) {
				mtx_unlock(&device->shader_slab_mutex);
				return s;
			}
		}
	}
	mtx_unlock(&device->shader_slab_mutex);

	return shader;
}

static void
radv_dump_faulty_shader(struct radv_device *device, uint64_t faulty_pc)
{
	struct radv_shader_variant *shader;
	uint64_t start_addr, end_addr;
	uint32_t instr_offset;

	shader = radv_get_faulty_shader(device, faulty_pc);
	if (!shader)
		return;

	start_addr = radv_buffer_get_va(shader->bo) + shader->bo_offset;
	end_addr = start_addr + shader->code_size;
	instr_offset = faulty_pc - start_addr;

	fprintf(stderr, "Faulty shader found "
			"VA=[0x%"PRIx64"-0x%"PRIx64"], instr_offset=%d\n",
		start_addr, end_addr, instr_offset);

	/* Get the list of instructions.
	 * Buffer size / 4 is the upper bound of the instruction count.
	 */
	unsigned num_inst = 0;
	struct radv_shader_inst *instructions =
		calloc(shader->code_size / 4, sizeof(struct radv_shader_inst));

	/* Split the disassembly string into instructions. */
	si_add_split_disasm(shader->disasm_string, start_addr, &num_inst, instructions);

	/* Print instructions with annotations. */
	for (unsigned i = 0; i < num_inst; i++) {
		struct radv_shader_inst *inst = &instructions[i];

		if (start_addr + inst->offset == faulty_pc) {
			fprintf(stderr, "\n!!! Faulty instruction below !!!\n");
			fprintf(stderr, "%s\n", inst->text);
			fprintf(stderr, "\n");
		} else {
			fprintf(stderr, "%s\n", inst->text);
		}
	}

	free(instructions);
}

struct radv_sq_hw_reg {
	uint32_t status;
	uint32_t trap_sts;
	uint32_t hw_id;
	uint32_t ib_sts;
};

static void
radv_dump_sq_hw_regs(struct radv_device *device)
{
	struct radv_sq_hw_reg *regs = (struct radv_sq_hw_reg *)&device->tma_ptr[6];

	fprintf(stderr, "\nHardware registers:\n");
	if (device->physical_device->rad_info.chip_class >= GFX10) {
		ac_dump_reg(stderr, device->physical_device->rad_info.chip_class,
			    R_000408_SQ_WAVE_STATUS, regs->status, ~0);
		ac_dump_reg(stderr, device->physical_device->rad_info.chip_class,
			    R_00040C_SQ_WAVE_TRAPSTS, regs->trap_sts, ~0);
		ac_dump_reg(stderr, device->physical_device->rad_info.chip_class,
			    R_00045C_SQ_WAVE_HW_ID1, regs->hw_id, ~0);
		ac_dump_reg(stderr, device->physical_device->rad_info.chip_class,
			    R_00041C_SQ_WAVE_IB_STS, regs->ib_sts, ~0);
	} else {
		ac_dump_reg(stderr, device->physical_device->rad_info.chip_class,
			    R_000048_SQ_WAVE_STATUS, regs->status, ~0);
		ac_dump_reg(stderr, device->physical_device->rad_info.chip_class,
			    R_00004C_SQ_WAVE_TRAPSTS, regs->trap_sts, ~0);
		ac_dump_reg(stderr, device->physical_device->rad_info.chip_class,
			    R_000050_SQ_WAVE_HW_ID, regs->hw_id, ~0);
		ac_dump_reg(stderr, device->physical_device->rad_info.chip_class,
			    R_00005C_SQ_WAVE_IB_STS, regs->ib_sts, ~0);
	}
	fprintf(stderr, "\n\n");
}

void
radv_check_trap_handler(struct radv_queue *queue)
{
	enum ring_type ring = radv_queue_family_to_ring(queue->queue_family_index);
	struct radv_device *device = queue->device;
	struct radeon_winsys *ws = device->ws;

	/* Wait for the context to be idle in a finite time. */
	ws->ctx_wait_idle(queue->hw_ctx, ring, queue->queue_idx);

	/* Try to detect if the trap handler has been reached by the hw by
	 * looking at ttmp0 which should be non-zero if a shader exception
	 * happened.
	 */
	if (!device->tma_ptr[4])
		return;

#if 0
	fprintf(stderr, "tma_ptr:\n");
	for (unsigned i = 0; i < 10; i++)
		fprintf(stderr, "tma_ptr[%d]=0x%x\n", i, device->tma_ptr[i]);
#endif

	radv_dump_sq_hw_regs(device);

	uint32_t ttmp0 = device->tma_ptr[4];
	uint32_t ttmp1 = device->tma_ptr[5];

	/* According to the ISA docs, 3.10 Trap and Exception Registers:
	 *
	 * "{ttmp1, ttmp0} = {3'h0, pc_rewind[3:0], HT[0], trapID[7:0], PC[47:0]}"
	 *
	 * "When the trap handler is entered, the PC of the faulting
	 *  instruction is: (PC - PC_rewind * 4)."
	 * */
	uint8_t trap_id = (ttmp1 >> 16) & 0xff;
	uint8_t ht = (ttmp1 >> 24) & 0x1;
	uint8_t pc_rewind = (ttmp1 >> 25) & 0xf;
	uint64_t pc = (ttmp0 | ((ttmp1 & 0x0000ffffull) << 32)) - (pc_rewind * 4);

	fprintf(stderr, "PC=0x%"PRIx64", trapID=%d, HT=%d, PC_rewind=%d\n",
		pc, trap_id, ht, pc_rewind);

	radv_dump_faulty_shader(device, pc);

	abort();
}
