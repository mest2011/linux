// SPDX-License-Identifier: GPL-2.0-only
/*
 * Minimal driver for the Maxim MAX98937 speaker amplifier, as found in the
 * Xiaomi Redmi Note 7 (lavender).
 *
 * The MAX98937 register map differs from the MAX98927. The init table comes
 * from the vendor GPL driver (techpack/audio/asoc/codecs/max98937.c). Without
 * the Android DSM speaker protection, the gains start at the vendor "safe"
 * values, and the control limits match the values Android uses.
 */
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/tlv.h>

#define MAX98937_PCM_MODE	0x0023
#define MAX98937_PCM_CLK	0x0025
#define MAX98937_PCM_SR1	0x0026
#define MAX98937_PCM_SR2	0x0027
#define MAX98937_AMP_VOL	0x0036
#define MAX98937_AMP_EN		0x0039
#define MAX98937_SPK_GAIN	0x003b
#define MAX98937_GLOBAL_EN	0x0500
#define MAX98937_REV_ID		0x0600

/* The parts seen so far report 0x43 here; the MAX98927 map reads 0x00. */
#define MAX98937_REV_MASK	0x40

struct max98937_priv {
	struct regmap *regmap;
	struct gpio_desc *reset;
};

static const struct reg_sequence max98937_init[] = {
	{ 0x0040, 0x01 },	/* Boost_Control_3 */
	{ 0x001e, 0x01 },	/* PCM_Tx_DOUT_Control_1 */
	{ 0x0018, 0x03 },	/* PCM_Rx_Enables_A */
	{ 0x001a, 0x00 },	/* PCM_Tx_Enables_A */
	{ 0x001c, 0xff },	/* PCM_Tx_HiZ_Control_A */
	{ 0x0028, 0x80 },	/* PCM_to_speaker_monomix_A */
	{ 0x0029, 0x01 },	/* PCM_to_speaker_monomix_B */
	{ 0x0011, 0x03 },	/* Clock_monitor_enable */
	{ 0x0053, 0x00 },	/* Brownout_level_infinite_hold */
	{ 0x0055, 0x00 },	/* Brownout_level_hold */
	{ 0x005e, 0x16 },	/* Brownout_level_1_current_limit */
	{ 0x005f, 0x00 },	/* Brownout_level_1_amp_1_control_1 */
	{ 0x0060, 0x0c },	/* Brownout_level_1_amp_1_control_2 */
	{ 0x0061, 0x0c },	/* Brownout_level_1_amp_1_control_3 */
	{ 0x0062, 0x12 },	/* Brownout_level_2_current_limit */
	{ 0x0063, 0x00 },	/* Brownout_level_2_amp_1_control_1 */
	{ 0x0064, 0x0c },	/* Brownout_level_2_amp_1_control_2 */
	{ 0x0065, 0x0c },	/* Brownout_level_2_amp_1_control_3 */
	{ 0x0066, 0x0e },	/* Brownout_level_3_current_limit */
	{ 0x0067, 0x03 },	/* Brownout_level_3_amp_1_control_1 */
	{ 0x0068, 0x18 },	/* Brownout_level_3_amp_1_control_2 */
	{ 0x0069, 0x18 },	/* Brownout_level_3_amp_1_control_3 */
	{ 0x006a, 0x0a },	/* Brownout_level_4_current_limit */
	{ 0x006b, 0x00 },	/* Brownout_level_4_amp_1_control_1 */
	{ 0x006c, 0x80 },	/* Brownout_level_4_amp_1_control_2 */
	{ 0x006d, 0x00 },	/* Brownout_level_4_amp_1_control_3 */
	{ 0x005a, 0x00 },	/* Brownout_threshold_hysteresis */
	{ 0x005b, 0x00 },	/* Brownout_AMP_limiter_attack_release */
	{ 0x005c, 0x1f },	/* Brownout_AMP_gain_attack_release */
	{ 0x005d, 0x00 },	/* Brownout_AMP1_clip_mode */
	{ 0x0042, 0x07 },	/* Meas_ADC_Config */
	{ 0x0014, 0x75 },	/* Meas_ADC_Thermal_Warning_Threshold */
	{ 0x0015, 0x8c },	/* Meas_ADC_Thermal_Shutdown_Threshold */
	{ 0x0017, 0x55 },	/* Pin_Config */
	{ 0x003e, 0xf7 },	/* Measurement_DSP_Config */
	{ 0x0023, 0x40 },	/* PCM_Mode_Config */
	{ 0x003a, 0x00 },	/* Speaker_source_select */
	{ 0x001b, 0x00 },	/* PCM_Tx_Enables_B */
	{ 0x0019, 0x00 },	/* PCM_Rx_Enables_B */
	{ 0x001f, 0x03 },	/* PCM_Tx_DOUT_Control_2 */
	{ 0x001d, 0xff },	/* PCM_Tx_HiZ_Control_B */
	{ 0x003d, 0x00 },	/* Measurement_enables */
	{ 0x0035, 0x00 },	/* PDM_Rx_Enable */
	{ 0x0036, 0x2c },	/* AMP_volume_control */
	{ 0x0037, 0x0f },	/* AMP_DSP_Config */
	{ 0x003b, 0x51 },	/* Speaker_Gain */
	{ 0x003c, 0x85 },	/* SSM_Configuration */
	{ 0x003f, 0x1c },	/* Boost_Control_0 */
	{ 0x0041, 0x40 },	/* Boost_Control_1 */
	{ 0x0043, 0x00 },	/* Meas_ADC_Base_Divide_MSByte */
	{ 0x0044, 0xff },	/* Meas_ADC_Base_Divide_LSByte */
	{ 0x0016, 0x08 },	/* Meas_ADC_Thermal_Hysteresis */
	{ 0x0073, 0x0f },	/* Env_Tracker_Vout_Headroom */
	{ 0x0077, 0x01 },	/* Env_Tracker_Control */
	{ 0x0052, 0x02 },	/* Brownout_enables */
	{ 0x0020, 0x00 },	/* PCM_STATUS_WORD_CONFIG_0 */
	{ 0x0021, 0x00 },	/* PCM_STATUS_WORD_CONFIG_1 */
	{ 0x0022, 0x00 },	/* PCM_STATUS_WORD_CONFIG_2 */
	{ 0x004e, 0x10 },	/* Squelch */
	{ 0x0079, 0x45 },	/* Boost_Bypass_1 */
	{ 0x007a, 0x2b },	/* Boost_Bypass_2 */
	{ 0x007b, 0x04 },	/* Boost_Bypass_3 */
};

static int max98937_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	if ((fmt & SND_SOC_DAIFMT_CLOCK_PROVIDER_MASK) != SND_SOC_DAIFMT_CBC_CFC)
		return -EINVAL;
	if ((fmt & SND_SOC_DAIFMT_FORMAT_MASK) != SND_SOC_DAIFMT_I2S)
		return -EINVAL;
	if ((fmt & SND_SOC_DAIFMT_INV_MASK) != SND_SOC_DAIFMT_NB_NF)
		return -EINVAL;
	return 0;
}

static int max98937_hw_params(struct snd_pcm_substream *substream,
			      struct snd_pcm_hw_params *params,
			      struct snd_soc_dai *dai)
{
	struct max98937_priv *p = snd_soc_component_get_drvdata(dai->component);
	unsigned int chansz, bsel, sr;

	switch (snd_pcm_format_width(params_format(params))) {
	case 16:
		chansz = 1;
		bsel = 2;
		break;
	case 24:
		chansz = 2;
		bsel = 3;
		break;
	case 32:
		chansz = 3;
		bsel = 4;
		break;
	default:
		return -EINVAL;
	}

	switch (params_rate(params)) {
	case 8000:
		sr = 0;
		break;
	case 11025:
		sr = 1;
		break;
	case 12000:
		sr = 2;
		break;
	case 16000:
		sr = 3;
		break;
	case 22050:
		sr = 4;
		break;
	case 24000:
		sr = 5;
		break;
	case 32000:
		sr = 6;
		break;
	case 44100:
		sr = 7;
		break;
	case 48000:
		sr = 8;
		break;
	default:
		return -EINVAL;
	}

	regmap_update_bits(p->regmap, MAX98937_PCM_MODE, 0xc0, chansz << 6);
	regmap_update_bits(p->regmap, MAX98937_PCM_CLK, 0x0f, bsel);
	regmap_update_bits(p->regmap, MAX98937_PCM_SR1, 0x0f, sr);
	regmap_update_bits(p->regmap, MAX98937_PCM_SR2, 0xf0, sr << 4);

	return 0;
}

static int max98937_mute(struct snd_soc_dai *dai, int mute, int stream)
{
	struct max98937_priv *p = snd_soc_component_get_drvdata(dai->component);

	if (mute) {
		regmap_update_bits(p->regmap, MAX98937_GLOBAL_EN, 1, 0);
		regmap_update_bits(p->regmap, MAX98937_AMP_EN, 1, 0);
	} else {
		regmap_update_bits(p->regmap, MAX98937_AMP_EN, 1, 1);
		regmap_update_bits(p->regmap, MAX98937_GLOBAL_EN, 1, 1);
	}

	return 0;
}

static const struct snd_soc_dai_ops max98937_dai_ops = {
	.set_fmt = max98937_set_fmt,
	.hw_params = max98937_hw_params,
	.mute_stream = max98937_mute,
	.no_capture_mute = 1,
};

static struct snd_soc_dai_driver max98937_dai = {
	.name = "max98937-aif1",
	.playback = {
		.stream_name = "HiFi Playback",
		.channels_min = 1,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_8000_48000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE |
			   SNDRV_PCM_FMTBIT_S32_LE,
	},
	.ops = &max98937_dai_ops,
};

static const DECLARE_TLV_DB_SCALE(max98937_spk_tlv, 300, 300, 0);
static const DECLARE_TLV_DB_SCALE(max98937_dig_tlv, -1600, 25, 0);

/* The maximums are the values Android uses with DSM speaker protection. */
static const struct snd_kcontrol_new max98937_controls[] = {
	SOC_SINGLE_TLV("Speaker Volume", MAX98937_SPK_GAIN, 0, 5, 0,
		       max98937_spk_tlv),
	SOC_SINGLE_TLV("Digital Volume", MAX98937_AMP_VOL, 0, 56, 0,
		       max98937_dig_tlv),
};

static const struct snd_soc_dapm_widget max98937_widgets[] = {
	SND_SOC_DAPM_DAC("Amp", "HiFi Playback", SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_OUTPUT("BE_OUT"),
};

static const struct snd_soc_dapm_route max98937_routes[] = {
	{ "BE_OUT", NULL, "Amp" },
};

static const struct snd_soc_component_driver max98937_component = {
	.controls = max98937_controls,
	.num_controls = ARRAY_SIZE(max98937_controls),
	.dapm_widgets = max98937_widgets,
	.num_dapm_widgets = ARRAY_SIZE(max98937_widgets),
	.dapm_routes = max98937_routes,
	.num_dapm_routes = ARRAY_SIZE(max98937_routes),
	.idle_bias_on = 1,
	.use_pmdown_time = 1,
	.endianness = 1,
};

static const struct regmap_config max98937_regmap = {
	.reg_bits = 16,
	.val_bits = 8,
	.max_register = MAX98937_REV_ID,
	.cache_type = REGCACHE_NONE,
};

static int max98937_probe(struct i2c_client *i2c)
{
	struct device *dev = &i2c->dev;
	struct max98937_priv *p;
	unsigned int rev;
	int ret;

	p = devm_kzalloc(dev, sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	i2c_set_clientdata(i2c, p);

	ret = devm_regulator_get_enable(dev, "vdd");
	if (ret)
		return dev_err_probe(dev, ret, "failed to enable vdd supply\n");

	p->regmap = devm_regmap_init_i2c(i2c, &max98937_regmap);
	if (IS_ERR(p->regmap))
		return dev_err_probe(dev, PTR_ERR(p->regmap),
				     "failed to init regmap\n");

	p->reset = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(p->reset))
		return dev_err_probe(dev, PTR_ERR(p->reset),
				     "failed to get reset GPIO\n");
	if (p->reset) {
		gpiod_set_value_cansleep(p->reset, 0);
		usleep_range(5000, 6000);
	}

	ret = regmap_read(p->regmap, MAX98937_REV_ID, &rev);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read revision\n");
	if (!(rev & MAX98937_REV_MASK))
		return dev_err_probe(dev, -ENODEV,
				     "unexpected revision 0x%02x\n", rev);
	dev_dbg(dev, "MAX98937 revision 0x%02x\n", rev);

	ret = regmap_multi_reg_write(p->regmap, max98937_init,
				     ARRAY_SIZE(max98937_init));
	if (ret)
		return dev_err_probe(dev, ret, "failed to write init table\n");

	return devm_snd_soc_register_component(dev, &max98937_component,
					       &max98937_dai, 1);
}

static const struct of_device_id max98937_of_match[] = {
	{ .compatible = "maxim,max98937" },
	{ }
};
MODULE_DEVICE_TABLE(of, max98937_of_match);

static const struct i2c_device_id max98937_id[] = {
	{ "max98937" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, max98937_id);

static struct i2c_driver max98937_driver = {
	.driver = {
		.name = "max98937",
		.of_match_table = max98937_of_match,
	},
	.probe = max98937_probe,
	.id_table = max98937_id,
};
module_i2c_driver(max98937_driver);

MODULE_DESCRIPTION("ASoC MAX98937 speaker amplifier driver");
MODULE_LICENSE("GPL");
