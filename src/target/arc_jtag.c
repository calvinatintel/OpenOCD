/***************************************************************************
 *   Copyright (C) 2013-2014 Synopsys, Inc.                                *
 *   Frank Dols <frank.dols@synopsys.com>                                  *
 *   Mischa Jonker <mischa.jonker@synopsys.com>                            *
 *   Anton Kolesov <anton.kolesov@synopsys.com>                            *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.           *
 ***************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "arc.h"

/* ----- Supporting functions ---------------------------------------------- */

typedef enum arc_jtag_reg_type {
	ARC_JTAG_CORE_REG,
	ARC_JTAG_AUX_REG
} reg_type_t;

/**
 * This functions sets instruction register in TAP. TAP end state is always
 * IRPAUSE.
 *
 * @param jtag_info
 * @param new_instr	Instruction to write to instruction register.
 */
static void arc_jtag_write_ir(struct arc_jtag *jtag_info, uint32_t
		new_instr)
{
	assert(jtag_info != NULL);
	assert(jtag_info->tap != NULL);

	struct jtag_tap *tap = jtag_info->tap;

	/* Set end state */
	jtag_info->tap_end_state = TAP_IRPAUSE;

	/* Do not set instruction if it is the same as current. */
	uint32_t current_instr = buf_get_u32(tap->cur_instr, 0, tap->ir_length);
	if (current_instr == new_instr)
		return;

	/* Create scan field to output new instruction. */
	struct scan_field field;
	uint8_t instr_buffer[4];
	field.num_bits = tap->ir_length;
	field.in_value = NULL;
	buf_set_u32(instr_buffer, 0, field.num_bits, new_instr);
	field.out_value = instr_buffer;

	/* From code in src/jtag/drivers/driver.c it look like that fields are
	 * copied so it is OK that field in this function is allocated in stack and
	 * thus this memory will be repurposed before jtag_queue_execute() will be
	 * invoked. */
	jtag_add_ir_scan(tap, &field, jtag_info->tap_end_state);
}

/**
 * Set transaction in command register. This function sets instruction register
 * and then transaction register, there is no need to invoke write_ir before
 * invoking this function.
 *
 * @param jtag_info
 * @param new_trans	Transaction to write to transaction command register.
 * @param end_state	End state after writing.
 */
static void arc_jtag_set_transaction(struct arc_jtag *jtag_info,
		arc_jtag_transaction_t new_trans, tap_state_t end_state)
{
	assert(jtag_info != NULL);
	assert(jtag_info->tap != NULL);

	/* No need to do anything. */
	if (jtag_info->cur_trans == new_trans)
		return;

	/* Set instruction. We used to call write_ir at upper levels, however
	 * write_ir-write_transaction were constantly in pair, so to avoid code
	 * duplication this function does it self. For this reasons it is "set"
	 * instead of "write". */
	arc_jtag_write_ir(jtag_info, ARC_TRANSACTION_CMD_REG);

	jtag_info->tap_end_state = end_state;

	const int num_bits[1] = { ARC_TRANSACTION_CMD_REG_LENGTH };
	const uint32_t values[1] = { new_trans };
	jtag_add_dr_out(jtag_info->tap, 1, num_bits, values, end_state);
	jtag_info->cur_trans = new_trans;
}

/**
 * Read 4-byte word from data register.
 *
 * Unlike arc_jtag_write_data, this function returns byte-buffer, caller must
 * convert this data to required format himself. This is done, because it is
 * impossible to convert data before jtag_queue_execute() is invoked, so it
 * cannot be done inside this function, so it has to operate with
 * byte-buffers. Write function on the other hand can "write-and-forget", data
 * is converted to byte-buffer before jtag_queue_execute().
 *
 * @param jtag_info
 * @param data		Array of bytes to read into.
 * @param end_state	End state after reading.
 */
static void arc_jtag_read_dr(struct arc_jtag *jtag_info, uint8_t *data,
		tap_state_t end_state)
{
	assert(jtag_info != NULL);
	assert(jtag_info->tap != NULL);

	jtag_info->tap_end_state = end_state;
	struct scan_field field;
	field.num_bits = 32;
	field.in_value = data;
	field.out_value = NULL;
	jtag_add_dr_scan(jtag_info->tap, 1, &field, jtag_info->tap_end_state);
}

/**
 * Write 4-byte word to data register.
 *
 * @param jtag_info
 * @param data		4-byte word to write into data register.
 * @param end_state	End state after writing.
 */
static void arc_jtag_write_dr(struct arc_jtag *jtag_info, uint32_t data,
		tap_state_t end_state)
{
	assert(jtag_info != NULL);
	assert(jtag_info->tap != NULL);

	jtag_info->tap_end_state = end_state;

	const int num_bits[1] = { 32 };
	const uint32_t values[1] = { data };
	jtag_add_dr_out(jtag_info->tap, 1, num_bits, values, end_state);
}

/**
 * Run us through transaction reset. This means that none of the previous
 * settings/commands/etc. are used anymore (of no influence).
 */
static void arc_jtag_reset_transaction(struct arc_jtag *jtag_info)
{
	arc_jtag_set_transaction(jtag_info, ARC_JTAG_CMD_NOP, TAP_IDLE);
}

/**
 * Write registers. addr is an array of addresses, and those addresses can be
 * in any order, though it is recommended that they are in sequential order
 * where possible, as this reduces number of JTAG commands to transfer.
 *
 * @param jtag_info
 * @param type		Type of registers to write: core or aux.
 * @param addr		Array of registers numbers.
 * @param count		Amount of registers in arrays.
 * @param values	Array of register values.
 */
static int arc_jtag_write_registers(struct arc_jtag *jtag_info, reg_type_t type,
	uint32_t *addr, uint32_t count, const uint32_t *buffer)
{
	int retval = ERROR_OK;
	unsigned int i;

	/*
	 * TODO: hacky hack, look for a proper way and logic (if there's) in setting
	 *       DEBUG bits, here the code completely ignored the clock gating one
	 *       and cleared it all the time... how could actionpoints have ever
	 *       worked in real silicon?!?!?
	 */
	if((*addr == 0x5) && (type == ARC_JTAG_AUX_REG)) {
		*((uint32_t*)buffer) |= 0x00100000;
		LOG_DEBUG(" ### forcing ED bit in DEBUG aux register");
	}

	LOG_DEBUG("Writing to %s registers: addr[0]=0x%" PRIu32 ";count=%" PRIu32
			  ";buffer[0]=0x%08" PRIx32,
		(type == ARC_JTAG_CORE_REG ? "core" : "aux"), *addr, count, *buffer);

	if (count == 0)
		return retval;

	arc_jtag_reset_transaction(jtag_info);

	/* What registers are we writing to? */
	const uint32_t transaction = (type == ARC_JTAG_CORE_REG ?
			ARC_JTAG_WRITE_TO_CORE_REG : ARC_JTAG_WRITE_TO_AUX_REG);
	arc_jtag_set_transaction(jtag_info, transaction, TAP_DRPAUSE);

	for (i = 0; i < count; i++) {
		/* Some of AUX registers are sequential, so we need to set address only
		 * for the first one in sequence. */
		if ( i == 0 || (addr[i] != addr[i-1] + 1) ) {
			arc_jtag_write_ir(jtag_info, ARC_ADDRESS_REG);
			arc_jtag_write_dr(jtag_info, addr[i], TAP_DRPAUSE);
			/* No need to set ir each time, but only if current ir is
			 * different. It is safe to put it into the if body, because this
			 * if is always executed in first iteration. */
			arc_jtag_write_ir(jtag_info, ARC_DATA_REG);
		}
		arc_jtag_write_dr(jtag_info, *(buffer + i), TAP_IDLE);
	}

	/* Cleanup. */
	arc_jtag_reset_transaction(jtag_info);

	/* Execute queue. */
	retval = jtag_execute_queue();
	if (ERROR_OK != retval) {
		LOG_ERROR("Writing to %s registers failed. Error code=%i",
			(type == ARC_JTAG_CORE_REG ? "core" : "aux"), retval);
		return retval;
	}

	return retval;
}

/**
 * Read registers. addr is an array of addresses, and those addresses can be in
 * any order, though it is recommended that they are in sequential order where
 * possible, as this reduces number of JTAG commands to transfer.
 *
 * @param jtag_info
 * @param type		Type of registers to read: core or aux.
 * @param addr		Array of registers numbers.
 * @param count		Amount of registers in arrays.
 * @param values	Array of register values.
 */
static int arc_jtag_read_registers(struct arc_jtag *jtag_info, reg_type_t type,
		uint32_t *addr, uint32_t count, uint32_t *buffer)
{
	int retval = ERROR_OK;
	uint32_t i;

	assert(jtag_info != NULL);
	assert(jtag_info->tap != NULL);

	LOG_DEBUG("Reading %s registers: addr[0]=0x%" PRIx32 ";count=%" PRIu32,
		(type == ARC_JTAG_CORE_REG ? "core" : "aux"), *addr, count);

	if (count == 0)
		return retval;

	arc_jtag_reset_transaction(jtag_info);

	/* What type of registers we are reading? */
	const uint32_t transaction = (type == ARC_JTAG_CORE_REG ?
			ARC_JTAG_READ_FROM_CORE_REG : ARC_JTAG_READ_FROM_AUX_REG);
	arc_jtag_set_transaction(jtag_info, transaction, TAP_DRPAUSE);

	struct scan_field *fields = calloc(sizeof(struct scan_field), count);
	uint8_t *data_buf = calloc(sizeof(uint8_t), count * 4);

	for (i = 0; i < count; i++) {
		/* Some of registers are sequential, so we need to set address only
		 * for the first one in sequence. */
		if (i == 0 || (addr[i] != addr[i-1] + 1)) {
			/* Set address of register */
			arc_jtag_write_ir(jtag_info, ARC_ADDRESS_REG);
			arc_jtag_write_dr(jtag_info, addr[i], TAP_IDLE);  // TAP_IDLE or TAP_DRPAUSE?
			arc_jtag_write_ir(jtag_info, ARC_DATA_REG);
		}

		arc_jtag_read_dr(jtag_info, data_buf + i * 4, TAP_IDLE);
	}

	/* Clean up */
	arc_jtag_reset_transaction(jtag_info);

	retval = jtag_execute_queue();
	if (ERROR_OK != retval) {
		LOG_ERROR("Reading from %s registers failed. Error code=%i",
			(type == ARC_JTAG_CORE_REG ? "core" : "aux"), retval);
		return retval;
	}

	/* Convert byte-buffers to host presentation. */
	for (i = 0; i < count; i++) {
		buffer[i] = buf_get_u32(data_buf + 4*i, 0, 32);
	}
	free(data_buf);
	free(fields);
	LOG_DEBUG("Read from register: buf[0]=0x%" PRIx32, buffer[0]);

	return retval;
}

/* ----- Exported JTAG functions ------------------------------------------- */

int arc_jtag_startup(struct arc_jtag *jtag_info)
{
	arc_jtag_reset_transaction(jtag_info);
	int retval = jtag_execute_queue();
	if (ERROR_OK != retval) {
		LOG_ERROR("Starting JTAG failed.");
		return retval;
	}
	return retval;
}

int arc_jtag_shutdown(struct arc_jtag *jtag_info)
{
	LOG_WARNING("arc_jtag_shutdown not implemented");
	return ERROR_OK;
}

/** Read STATUS register. */
int arc_jtag_status(struct arc_jtag * const jtag_info, uint32_t * const value)
{
	assert(jtag_info != NULL);
	assert(jtag_info->tap != NULL);

	int retval = ERROR_OK;
	uint8_t buffer[4];

	/* Fill command queue. */
	arc_jtag_reset_transaction(jtag_info);
	arc_jtag_write_ir(jtag_info, ARC_JTAG_STATUS_REG);
	arc_jtag_read_dr(jtag_info, buffer, TAP_IDLE);
	arc_jtag_reset_transaction(jtag_info);

	/* Execute queue. */
	retval = jtag_execute_queue();
	if (ERROR_OK != retval) {
		LOG_ERROR("Reading STATUS register failed. Error code = %i", retval);
		return retval;
	}

	/* Parse output. */
	*value = buf_get_u32(buffer, 0, 32);

	return retval;
}

/** Read IDCODE register. */
int arc_jtag_idcode(struct arc_jtag * const jtag_info, uint32_t * const value)
{
	assert(jtag_info != NULL);
	assert(jtag_info->tap != NULL);

	LOG_DEBUG("Reading IDCODE register.");

	int retval = ERROR_OK;
	uint8_t buffer[4];

	/* Fill command queue. */
	arc_jtag_reset_transaction(jtag_info);
	arc_jtag_write_ir(jtag_info, ARC_IDCODE_REG);
	arc_jtag_read_dr(jtag_info, buffer, TAP_IDLE);
	arc_jtag_reset_transaction(jtag_info);

	/* Execute queue. */
	retval = jtag_execute_queue();
	if (ERROR_OK != retval) {
		LOG_ERROR("Reading IDCODE register failed. Error code = %i", retval);
		return retval;
	}

	/* Parse output. */
	*value = buf_get_u32(buffer, 0, 32);
	LOG_DEBUG("IDCODE register=0x%08" PRIx32, *value);

	return retval;
}

/**
 * Write a sequence of 4-byte words into target memory.
 *
 * We can write only 4byte words via JTAG, so any non-word writes should be
 * handled at higher levels by read-modify-write.
 *
 * This function writes directly to the memory, leaving any caches (if there
 * are any) in inconsistent state. It is responsibility of upper level to
 * resolve this.
 *
 * @param jtag_info
 * @param addr		Address of first word to write into.
 * @param count		Amount of word to write.
 * @param buffer	Array to write into memory.
 */
int arc_jtag_write_memory(struct arc_jtag *jtag_info, uint32_t addr,
		uint32_t count, const uint32_t* buffer)
{
	int retval = ERROR_OK;

	assert(jtag_info != NULL);
	assert(buffer != NULL);

	LOG_DEBUG("Writing to memory: addr=0x%08" PRIx32 ";count=%" PRIu32 ";buffer[0]=0x%08" PRIx32,
		addr, count, *buffer);

	/* No need to waste time on useless operations. */
	if (count == 0)
		return retval;

	/* We do not know where we come from. */
	arc_jtag_reset_transaction(jtag_info);

	/* We want to write to memory. */
	arc_jtag_set_transaction(jtag_info, ARC_JTAG_WRITE_TO_MEMORY, TAP_DRPAUSE);

	/* Set target memory address of the first word. */
	arc_jtag_write_ir(jtag_info, ARC_ADDRESS_REG);
	arc_jtag_write_dr(jtag_info, addr, TAP_DRPAUSE);

	/* Start sending words. Address is auto-incremented on 4bytes by HW. */
	arc_jtag_write_ir(jtag_info, ARC_DATA_REG);
	uint32_t i;
	for (i = 0; i < count; i++) {
		arc_jtag_write_dr(jtag_info, *(buffer + i), TAP_IDLE);
	}

	/* Cleanup. */
	arc_jtag_reset_transaction(jtag_info);

	/* Run queue. */
	retval = jtag_execute_queue();
	if (ERROR_OK != retval) {
		LOG_ERROR("Writing to memory failed. Error code = %i", retval);
		return retval;
	}

	return retval;
}

/**
 * Read a sequence of 4-byte words from target memory.
 *
 * We can read only 4byte words via JTAG.
 *
 * This function read directly from the memory, so it can read invalid data if
 * data cache hasn't been flushed before hand. It is responsibility of upper
 * level to resolve this.
 *
 * @param jtag_info
 * @param addr		Address of first word to read from.
 * @param count		Amount of words to read.
 * @param buffer	Array of words to read into.
 */
int arc_jtag_read_memory(struct arc_jtag *jtag_info, uint32_t addr,
	uint32_t count, uint32_t *buffer )
{
	int retval = ERROR_OK;

	assert(jtag_info != NULL);
	assert(jtag_info->tap != NULL);

	LOG_DEBUG("Reading memory: addr=0x%" PRIx32 ";count=%" PRIu32, addr, count);

	if (count == 0)
		return retval;

	arc_jtag_reset_transaction(jtag_info);

	/* We are reading from memory. */
	arc_jtag_set_transaction(jtag_info, ARC_JTAG_READ_FROM_MEMORY, TAP_DRPAUSE);

	/* Set address of the first word */
	arc_jtag_write_ir(jtag_info, ARC_ADDRESS_REG);
	arc_jtag_write_dr(jtag_info, addr, TAP_IDLE);

	/* Read data */
	arc_jtag_write_ir(jtag_info, ARC_DATA_REG);
	uint8_t *data_buf = calloc(sizeof(uint8_t), count * 4);
	uint32_t i;
	for (i = 0; i < count; i++) {
		arc_jtag_read_dr(jtag_info, data_buf + i * 4, TAP_IDLE);
	}

	/* Clean up */
	arc_jtag_reset_transaction(jtag_info);

	retval = jtag_execute_queue();
	if (ERROR_OK != retval) {
		LOG_ERROR("Reading from memory failed. Error code=%i", retval);
		return retval;
	}

	/* Convert byte-buffers to host presentation. */
	for (i = 0; i < count; i++) {
		buffer[i] = buf_get_u32(data_buf + 4*i, 0, 32);
	}

	free(data_buf);

	return retval;
}

/** Wrapper function to ease writing of one core register. */
int arc_jtag_write_core_reg_one(struct arc_jtag *jtag_info, uint32_t addr,
	uint32_t value)
{
	return arc_jtag_write_core_reg(jtag_info, &addr, 1, &value);
}

/**
 * Write core registers. addr is an array of addresses, and those addresses can
 * be in any order, though it is recommended that they are in sequential order
 * where possible, as this reduces number of JTAG commands to transfer.
 *
 * @param jtag_info
 * @param addr		Array of registers numbers.
 * @param count		Amount of registers in arrays.
 * @param values	Array of register values.
 */
int arc_jtag_write_core_reg(struct arc_jtag *jtag_info, uint32_t* addr,
	uint32_t count, const uint32_t* buffer)
{
	return arc_jtag_write_registers(jtag_info, ARC_JTAG_CORE_REG, addr, count,
			buffer);
}

/** Wrapper function to ease reading of one core register. */
int arc_jtag_read_core_reg_one(struct arc_jtag *jtag_info, uint32_t addr,
	uint32_t *value)
{
	return arc_jtag_read_core_reg(jtag_info, &addr, 1, value);
}

/**
 * Read core registers. addr is an array of addresses, and those addresses can
 * be in any order, though it is recommended that they are in sequential order
 * where possible, as this reduces number of JTAG commands to transfer.
 *
 * @param jtag_info
 * @param addr		Array of core register numbers.
 * @param count		Amount of registers in arrays.
 * @param values	Array of register values.
 */
int arc_jtag_read_core_reg(struct arc_jtag *jtag_info, uint32_t *addr,
	uint32_t count, uint32_t* buffer)
{
	return arc_jtag_read_registers(jtag_info, ARC_JTAG_CORE_REG, addr, count,
			buffer);
}

/** Wrapper function to ease writing of one AUX register. */
int arc_jtag_write_aux_reg_one(struct arc_jtag *jtag_info, uint32_t addr,
	uint32_t value)
{
	return arc_jtag_write_aux_reg(jtag_info, &addr, 1, &value);
}

/**
 * Write AUX registers. addr is an array of addresses, and those addresses can
 * be in any order, though it is recommended that they are in sequential order
 * where possible, as this reduces number of JTAG commands to transfer.
 *
 * @param jtag_info
 * @param addr		Array of registers numbers.
 * @param count		Amount of registers in arrays.
 * @param values	Array of register values.
 */
int arc_jtag_write_aux_reg(struct arc_jtag *jtag_info, uint32_t* addr,
	uint32_t count, const uint32_t* buffer)
{
	return arc_jtag_write_registers(jtag_info, ARC_JTAG_AUX_REG, addr, count,
			buffer);
}

/** Wrapper function to ease reading of one AUX register. */
int arc_jtag_read_aux_reg_one(struct arc_jtag *jtag_info, uint32_t addr,
	uint32_t *value)
{
	return arc_jtag_read_aux_reg(jtag_info, &addr, 1, value);
}

/**
 * Read AUX registers. addr is an array of addresses, and those addresses can
 * be in any order, though it is recommended that they are in sequential order
 * where possible, as this reduces number of JTAG commands to transfer.
 *
 * @param jtag_info
 * @param addr		Array of AUX register numbers.
 * @param count		Amount of registers in arrays.
 * @param values	Array of register values.
 */
int arc_jtag_read_aux_reg(struct arc_jtag *jtag_info, uint32_t *addr,
	uint32_t count, uint32_t* buffer)
{
	return arc_jtag_read_registers(jtag_info, ARC_JTAG_AUX_REG, addr, count,
			buffer);
}

