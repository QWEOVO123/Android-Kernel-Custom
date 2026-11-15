/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef QWEOVO_SUKISU_GRANT_H
#define QWEOVO_SUKISU_GRANT_H

#include <crypto/hash.h>
#include <crypto/public_key.h>
#include <linux/random.h>

#define SUKISU_GRANT_MAGIC "MEOWSU1"
#define SUKISU_DIGEST_SIZE 32
#define SUKISU_SIGNATURE_SIZE 256
#define SUKISU_RESPONSE_PREFIX "MEOW-SU1-SIG:"

/* All fields are byte arrays. The signed message ends before signature. */
struct sukisu_grant {
	u8 magic[sizeof(SUKISU_GRANT_MAGIC)];
	u8 key_id[SUKISU_DIGEST_SIZE];
	u8 nonce[SUKISU_DIGEST_SIZE];
	u8 signature[SUKISU_SIGNATURE_SIZE];
};

#ifndef QWEOVO_SUKISU_PUBLIC_KEY
static const unsigned char sukisu_public_key[] = {};
#endif

static int sukisu_sha256(const void *bytes, unsigned int size, u8 *digest)
{
	struct crypto_shash *tfm;
	int ret;

	tfm = crypto_alloc_shash("sha256", 0, 0);
	if (IS_ERR(tfm))
		return PTR_ERR(tfm);
	ret = crypto_shash_tfm_digest(tfm, bytes, size, digest);
	crypto_free_shash(tfm);
	return ret;
}

static int sukisu_verify_grant(struct sukisu_grant *grant)
{
	u8 digest[SUKISU_DIGEST_SIZE];
	struct public_key key = {
		.key = (void *)sukisu_public_key,
		.keylen = sizeof(sukisu_public_key),
		.algo = OID_rsaEncryption,
		.pkey_algo = "rsa",
	};
	struct public_key_signature sig = {
		.s = grant->signature,
		.s_size = sizeof(grant->signature),
		.digest = digest,
		.digest_size = sizeof(digest),
		.pkey_algo = "rsa",
		.hash_algo = "sha256",
		.encoding = "pkcs1",
	};
	int ret;

	ret = sukisu_sha256(grant, offsetof(struct sukisu_grant, signature), digest);
	return ret ? ret : public_key_verify_signature(&key, &sig);
}

static int sukisu_write_grant(struct file *store, const struct sukisu_grant *grant)
{
	const struct cred *old = override_creds(store->f_cred);
	loff_t pos = 0;
	ssize_t written;
	int ret = 0;

	file_start_write(store);
	written = __kernel_write(store, grant, sizeof(*grant), &pos);
	/* Never leave a stale tail behind: a longer file is rejected at boot. */
	if (written == sizeof(*grant) &&
	    i_size_read(file_inode(store)) > sizeof(*grant)) {
		ret = vfs_truncate(&store->f_path, sizeof(*grant));
		if (ret)
			written = ret;
	}
	file_end_write(store);
	if (written == sizeof(*grant))
		ret = vfs_fsync(store, 0);
	else
		ret = written < 0 ? written : -EIO;
	revert_creds(old);
	return ret;
}

static int sukisu_rotate_grant(struct file *store, struct sukisu_grant *next,
			       bool *allow)
{
	struct sukisu_grant previous;
	loff_t pos = 0;
	ssize_t size;
	int ret;
	bool verified = false;

	memset(next, 0, sizeof(*next));
	memcpy(next->magic, SUKISU_GRANT_MAGIC, sizeof(next->magic));
	ret = sukisu_sha256(sukisu_public_key, sizeof(sukisu_public_key), next->key_id);
	if (ret)
		return ret;
	ret = get_random_bytes_wait(next->nonce, sizeof(next->nonce));
	if (ret)
		return ret;

	size = __kernel_read(store, &previous, sizeof(previous), &pos);
	if (size < 0)
		return size;
	if (size == sizeof(previous) &&
	    !memcmp(previous.magic, next->magic, sizeof(previous.magic)) &&
	    !memcmp(previous.key_id, next->key_id, sizeof(previous.key_id)) &&
	    memchr_inv(previous.signature, 0, sizeof(previous.signature))) {
		verified = !sukisu_verify_grant(&previous);
		if (!verified)
			pr_info("qweovo_bootmenu: SukiSU grant signature rejected; rotating challenge\n");
	}

	/* Rotate even without a grant. Never unlock before the old one is gone. */
	ret = sukisu_write_grant(store, next);
	if (!ret)
		*allow = verified;
	return ret;
}

/* Normal boots keep the stored challenge, including an armed signature, so a
 * grant armed in Android survives until the menu boot actually consumes it.
 */
static int sukisu_ensure_grant(struct file *store, struct sukisu_grant *grant,
			       bool *allow)
{
	struct sukisu_grant previous;
	u8 key_id[SUKISU_DIGEST_SIZE];
	loff_t pos = 0;
	ssize_t size;
	int ret;

	*allow = false;
	ret = sukisu_sha256(sukisu_public_key, sizeof(sukisu_public_key), key_id);
	if (ret)
		return ret;
	size = __kernel_read(store, &previous, sizeof(previous), &pos);
	if (size == sizeof(previous) &&
	    !memcmp(previous.magic, SUKISU_GRANT_MAGIC, sizeof(previous.magic)) &&
	    !memcmp(previous.key_id, key_id, sizeof(previous.key_id))) {
		*grant = previous;
		return 0;
	}
	return sukisu_rotate_grant(store, grant, allow);
}

#endif
