package feed.api;

import java.util.Arrays;

/**
 * What a request in flight needs until its result is rendered, per client slot: the post id, the user id and, for a
 * new post, its body and author. Arrays rather than an object per request, so steady state allocates nothing.
 */
final class RequestArgs {

    private long[] postIds = new long[0];
    private long[] userIds = new long[0];
    private byte[][] texts = new byte[0][]; // a new post's body, then its author
    private int[] bodyLengths = new int[0];

    long postId(int slot) {
        return postIds[slot];
    }

    long userId(int slot) {
        return userIds[slot];
    }

    void setPost(int slot, long postId) {
        ensure(slot);
        postIds[slot] = postId;
    }

    void setLike(int slot, long userId, long postId) {
        ensure(slot);
        userIds[slot] = userId;
        postIds[slot] = postId;
    }

    /** A new post: copies the body and the author (the copy lives only until the insert answers). */
    void setNewPost(int slot, long userId, byte[] body, int bodyOff, int bodyLength, byte[] author, int authorLength) {
        ensure(slot);
        byte[] text = new byte[bodyLength + authorLength];
        System.arraycopy(body, bodyOff, text, 0, bodyLength);
        System.arraycopy(author, 0, text, bodyLength, authorLength);
        userIds[slot] = userId;
        texts[slot] = text;
        bodyLengths[slot] = bodyLength;
    }

    /** The new post's body is {@code text(slot)[0, bodyLength(slot))}; its author is the rest. */
    byte[] text(int slot) {
        return texts[slot];
    }

    int bodyLength(int slot) {
        return bodyLengths[slot];
    }

    void clear(int slot) {
        if (slot < texts.length) {
            texts[slot] = null;
        }
    }

    private void ensure(int slot) {
        if (slot >= postIds.length) {
            int n = Math.max(slot + 1, Math.max(1024, postIds.length * 2));
            postIds = Arrays.copyOf(postIds, n);
            userIds = Arrays.copyOf(userIds, n);
            texts = Arrays.copyOf(texts, n);
            bodyLengths = Arrays.copyOf(bodyLengths, n);
        }
    }
}
