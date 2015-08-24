#include "ccx_dtvcc.h"
#include "ccx_common_common.h"
#include "ccx_encoders_common.h"
#include "ccx_decoders_708_output.h"

void ccx_dtvcc_process_data(struct lib_cc_decode *ctx, const unsigned char *data, int data_length)
{
	/*
	 * Note: the data has following format:
	 * 1 byte for cc_valid
	 * 1 byte for cc_type
	 * 2 bytes for the actual data
	 */

	ccx_dtvcc_ctx_t *dtvcc = ctx->dtvcc;

	if (!dtvcc->is_active && !dtvcc->report_enabled)
		return;

	for (int i = 0; i < data_length; i += 4)
	{
		unsigned char cc_valid = data[i];
		unsigned char cc_type = data[i + 1];

		switch (cc_type)
		{
			case 2:
				ccx_common_logging.debug_ftn (CCX_DMT_708, "[CEA-708] dtvcc_process_data: DTVCC Channel Packet Data\n");
				if (cc_valid == 0) // This ends the previous packet
					dtvcc_process_current_packet(dtvcc);
				else
				{
					if (dtvcc->current_packet_length > 253)
					{
						ccx_common_logging.debug_ftn(CCX_DMT_708, "[CEA-708] dtvcc_process_data: "
								"Warning: Legal packet size exceeded (1), data not added.\n");
					}
					else
					{
						dtvcc->current_packet[dtvcc->current_packet_length++] = data[i + 2];
						dtvcc->current_packet[dtvcc->current_packet_length++] = data[i + 3];
					}
				}
				break;
			case 3:
				ccx_common_logging.debug_ftn (CCX_DMT_708, "[CEA-708] dtvcc_process_data: DTVCC Channel Packet Start\n");
				dtvcc_process_current_packet(dtvcc);
				if (cc_valid)
				{
					if (dtvcc->current_packet_length > DTVCC_MAX_PACKET_LENGTH - 1)
					{
						ccx_common_logging.debug_ftn(CCX_DMT_708, "[CEA-708] dtvcc_process_data: "
								"Warning: Legal packet size exceeded (2), data not added.\n");
					}
					else
					{
						dtvcc->current_packet[dtvcc->current_packet_length++] = data[i + 2];
						dtvcc->current_packet[dtvcc->current_packet_length++] = data[i + 3];
					}
				}
				break;
			default:
				ccx_common_logging.fatal_ftn (CCX_COMMON_EXIT_BUG_BUG, "[CEA-708] dtvcc_process_data: "
						"shouldn't be here - cc_type: %d\n", cc_type);
		}
	}
}

//--------------------------------------------------------------------------------------

ccx_dtvcc_ctx_t *ccx_dtvcc_init(struct ccx_decoder_dtvcc_settings_t *opts)
{
	ccx_common_logging.debug_ftn(CCX_DMT_708, "[CEA-708] initializing dtvcc decoder\n");
	ccx_dtvcc_ctx_t *ctx = (ccx_dtvcc_ctx_t *) malloc(sizeof(ccx_dtvcc_ctx_t));
	if (!ctx)
		ccx_common_logging.fatal_ftn(EXIT_NOT_ENOUGH_MEMORY, "[CEA-708] ccx_dtvcc_init");

	ctx->report = opts->report;
	ctx->report->reset_count = 0;
	ctx->is_active = 0;
	ctx->report_enabled = 0;
	ctx->active_services_count = opts->active_services_count;

	memcpy(ctx->services_active, opts->services_enabled, DTVCC_MAX_SERVICES * sizeof(int));

	_dtvcc_clear_packet(ctx);

	ctx->last_sequence = DTVCC_NO_LAST_SEQUENCE;

	ctx->report_enabled = opts->print_file_reports;
	ctx->encoder = init_encoder(opts->enc_cfg);
	ctx->timing = opts->timing;

	for (int i = 0; i < DTVCC_MAX_SERVICES; i++)
	{
		if (!ctx->services_active[i])
			continue;

		dtvcc_service_decoder *decoder = &ctx->decoders[i];

		decoder->fh = -1;
		decoder->cc_count = 0;
		decoder->filename = NULL;
		decoder->output_started = 0;
		decoder->output_format = opts->output_format;

		char *enc = opts->all_services_charset ?
					opts->all_services_charset :
					opts->services_charsets[i];

		if (enc)
		{
			decoder->charset = strdup(enc);
			decoder->cd = iconv_open("UTF-8", decoder->charset);
			if (decoder->cd == (iconv_t) -1)
			{
				ccx_common_logging.fatal_ftn(EXIT_FAILURE, "[CEA-708] dtvcc_init: "
													 "can't create iconv for charset \"%s\": %s\n",
											 decoder->charset, strerror(errno));
			}
		}
		else
		{
			decoder->charset = NULL;
			decoder->cd = (iconv_t) -1;
		}

		_dtvcc_windows_reset(decoder);

		_dtvcc_decoder_init_write(decoder, opts->basefilename, i + 1);

		if (opts->cc_to_stdout)
		{
			decoder->fh = STDOUT_FILENO;
			decoder->output_started = 1;
		}
	}

	return ctx;
}

void ccx_dtvcc_free(ccx_dtvcc_ctx_t **ctx_ptr)
{
	ccx_common_logging.debug_ftn(CCX_DMT_708, "[CEA-708] dtvcc_free: cleaning up\n");

	ccx_dtvcc_ctx_t *ctx = *ctx_ptr;

	for (int i = 0; i < DTVCC_MAX_SERVICES; i++)
	{
		if (!ctx->services_active[i])
			continue;

		dtvcc_service_decoder *decoder = &ctx->decoders[i];

		if (decoder->output_started)
		{
			current_field = 3;
			_dtvcc_decoder_flush(ctx, decoder);
		}

		if (decoder->charset)
		{
			iconv_close(decoder->cd);
			free(decoder->charset);
		}

		if (decoder->fh != -1 && decoder->fh != STDOUT_FILENO)
		{
			ccx_dtvcc_write_done(decoder, ctx->encoder);
			close(decoder->fh);
		}

		free(decoder->filename);

		for (int j = 0; j < DTVCC_MAX_WINDOWS; j++)
			if (decoder->windows[j].memory_reserved)
			{
				for (int k = 0; k < DTVCC_MAX_ROWS; k++)
					free(decoder->windows[j].rows[k]);
				decoder->windows[j].memory_reserved = 0;
			}
	}
	freep(ctx_ptr);
}