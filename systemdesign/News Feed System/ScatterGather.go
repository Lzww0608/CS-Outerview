package main

import (
	"context"
	"fmt"
	"time"

	"golang.org/x/sync/errgroup"
	"golang.org/x/sync/singleflight"
)

type Post struct {
	ID       int64
	Content  string
	AuthorID int64
}

type User struct {
	ID   int64
	Name string
}

type FeedItem struct {
	Post *Post
	User *User
}

var requestGroup singleflight.Group

func fectchPostWithDedup(id int64) (*Post, error) {
	key := fmt.Sprintf("post_%d", id)

	v, err, _ := requestGroup.Do(key, func() (any, error) {
		return fetchPost(context.Background(), id)
	})

	if err != nil {
		return nil, err
	}

	return v.(*Post), nil
}

func fectchUserWithDedup(id int64) (*User, error) {
	key := fmt.Sprintf("user_%d", id)

	v, err, _ := requestGroup.Do(key, func() (any, error) {
		return fetchUser(context.Background(), id)
	})

	return v.(*User), err
}

func fetchPost(ctx context.Context, id int64) (*Post, error) {
	time.Sleep(10 * time.Millisecond)
	return &Post{ID: id, Content: "Hello World", AuthorID: 100 + id}, nil
}

func fetchUser(ctx context.Context, id int64) (*User, error) {
	time.Sleep(10 * time.Millisecond)
	return &User{ID: id, Name: fmt.Sprintf("User_%d", id)}, nil
}

func GetNewsFeed(ctx context.Context, postIDs []int64) ([]*FeedItem, error) {
	g, ctx := errgroup.WithContext(ctx)

	res := make([]*FeedItem, len(postIDs))

	for i, id := range postIDs {
		idx, pid := i, id

		g.Go(func() error {
			post, err := fectchPostWithDedup(pid)
			if err != nil {
				return err
			}

			user, err := fectchUserWithDedup(post.AuthorID)
			if err != nil {
				return err
			}

			res[idx] = &FeedItem{Post: post, User: user}
			return nil
		})
	}

	if err := g.Wait(); err != nil {
		return nil, err
	}

	return res, nil
}

func main() {
	ids := []int64{1, 2, 3, 4, 5}
	start := time.Now()

	items, err := GetNewsFeed(context.Background(), ids)
	if err != nil {
		panic(err)
	}

	fmt.Printf("耗时: %v (如果串行需要 100ms+)\n", time.Since(start))
	for _, item := range items {
		fmt.Printf("Post: %d, Author: %s\n", item.Post.ID, item.User.Name)
	}
}
