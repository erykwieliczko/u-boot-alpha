// SPDX-License-Identifier: GPL-2.0+
#include <blk.h>
#include <command.h>
#include <dm.h>
#include <dm/device-internal.h>
#include <dm/root.h>
#include <dm/test.h>
#include <test/test.h>
#include <test/ut.h>

struct flush_test_priv {
	unsigned int calls;
	int result;
};

static int flush_test_flush(struct udevice *dev)
{
	struct flush_test_priv *priv = dev_get_priv(dev);

	priv->calls++;
	return priv->result;
}

static ulong flush_test_read(struct udevice *dev, lbaint_t start,
			     lbaint_t count, void *buffer)
{
	return 0;
}

static const struct blk_ops flush_test_ops = {
	.read = flush_test_read,
	.flush = flush_test_flush,
};

U_BOOT_DRIVER(nvme_flush_test_blk) = {
	.name = "nvme_flush_test_blk",
	.id = UCLASS_BLK,
	.ops = &flush_test_ops,
	.priv_auto = sizeof(struct flush_test_priv),
};

static int dm_test_nvme_flush(struct unit_test_state *uts)
{
	struct udevice *first, *second;
	struct flush_test_priv *a, *b;

	ut_assertok(blk_create_devicef(dm_root(), "nvme_flush_test_blk", "first",
				      UCLASS_NVME, 0, 4096, 0, &first));
	ut_assertok(blk_create_devicef(dm_root(), "nvme_flush_test_blk", "second",
				      UCLASS_NVME, 1, 4096, 0, &second));
	ut_assertok(device_probe(first));
	ut_assertok(device_probe(second));
	a = dev_get_priv(first);
	b = dev_get_priv(second);

	ut_assertok(run_command("nvme device 1", 0));
	ut_assertok(run_command("nvme flush", 0));
	ut_asserteq(0, a->calls);
	ut_asserteq(1, b->calls);
	b->result = -ETIMEDOUT;
	ut_asserteq(CMD_RET_FAILURE, run_command("nvme flush", 0));
	ut_asserteq(2, b->calls);
	b->result = -EIO;
	ut_asserteq(CMD_RET_FAILURE, run_command("nvme flush", 0));
	ut_asserteq(3, b->calls);
	ut_assertok(run_command("nvme device 0", 0));
	ut_assertok(run_command("nvme flush", 0));
	ut_asserteq(1, a->calls);
	ut_assertok(device_remove(second, DM_REMOVE_NORMAL));
	ut_assertok(device_unbind(second));
	ut_assertok(device_remove(first, DM_REMOVE_NORMAL));
	ut_assertok(device_unbind(first));
	ut_asserteq(CMD_RET_FAILURE, run_command("nvme flush", 0));

	return 0;
}
DM_TEST(dm_test_nvme_flush, UTF_SCAN_PDATA | UTF_SCAN_FDT | UTF_CONSOLE);
