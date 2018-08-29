#include <stdio.h>
#include "texture.h"
#include <Eigen/Core>
#include <Eigen/Geometry>

#include <acc/bvh_tree.h>


Graph::Graph(int _faces_num, int _views_num) :faces_num(_faces_num), views_num(_views_num) {
	data_costs.resize(_faces_num);
	tex_loop(i, data_costs.size()) {
		data_costs[i].resize(_views_num + 1, 1.0f);
	}
	valid_mask.resize(_faces_num);
	tex_loop(i, valid_mask.size()) {
		valid_mask[i].resize(_views_num, 0);
	}
}

// calculate area of a face project to a certion view
float get_face_area(const Model& model, const std::vector<Eigen::Vector2f>& uvs, int face, const View& view)
{
	Eigen::Vector2f uv[3];
	tex_loop(i, 3) {
		const int& vertex = model.faces[10 * face + i];
		uv[i] = uvs[vertex];
		if (uv[i][0] >= view.img.cols || uv[i][0] < 0 ||
			uv[i][1] >= view.img.rows || uv[i][1] < 0)
		{
			return -1.0f;
		}
	}
	Eigen::Vector2f ab = uv[1] - uv[0];
	Eigen::Vector2f ac = uv[2] - uv[0];
	float face_area = fabs(ab[0] * ac[1] - ab[1] * ac[0]) * 0.5f;
	return face_area;
}

// change data structure for loading into mapMAP solver
void eigen_to_math(Eigen::Vector3f eigen_vec, math::Vec3f& math_vec) {
	tex_loop(k, 3) {
		math_vec[k] = eigen_vec[k];
	}
}

// 3D coordinates to 2D(image) coordinates
Eigen::Vector2f find_uv(const Eigen::Vector3f& vertex, const View& view)
{
	const Eigen::Matrix3f& R = view.R;
	const Eigen::Vector3f& t = view.t;
	const Eigen::Matrix3f& K = view.K;

	Eigen::Vector3f p_cam = R * vertex + t;
	Eigen::Vector3f p_img = K * p_cam;
	p_img /= p_img(2);

	Eigen::Vector2f uv;
	uv(0) = p_img(0);
	uv(1) = p_img(1);
	return uv;
}

// calculate all uv coordinates on a certaim view
void calculate_uvs(const Model& model, const View& view, std::vector<Eigen::Vector2f>& uvs) {
	uvs.resize(model.vertices.size());
	tex_loop(i, model.vertices.size()) {
		uvs[i] = find_uv(model.vertices[i], view);
	}
}

// calculate face normal
Eigen::Vector3f get_face_normal(const Model& model, int face)
{
	Eigen::Vector3f vertices[3];
	tex_loop(i, 3) {
		const int &vertex = model.faces[10 * face + i];
		vertices[i] = model.vertices[vertex];
	}
	Eigen::Vector3f face_normal = (vertices[0] - vertices[1]).cross(vertices[0] - vertices[2]);

	Eigen::Vector3f sum_vertex_normals = { 0.0f, 0.0f, 0.0f };
	tex_loop(i, 3) {
		const int &normal = model.faces[10 * face + 3 + i];
		sum_vertex_normals += model.normals[normal];
	}
	if (face_normal.dot(sum_vertex_normals) < 0) {
		face_normal = -face_normal;
	}
	face_normal.normalize();
	return face_normal;
}

// calculate data term of the energy function 
int Graph::calculate_data_cost(const Model& model, const std::vector<View>& views)
{
	b_good_face.resize(faces_num, false);

	std::vector<std::vector<float>> face_areas(faces_num, std::vector<float>(views_num, 0.0f));
	float max_area = 0.0f;
	std::vector<Eigen::Vector3f> view_direction(views_num, Eigen::Vector3f(0.0f, 0.0f, 0.0f));
	std::vector<math::Vec3f> view_pos(views_num);
	typedef acc::BVHTree<unsigned int, math::Vec3f> BVHTree;

	std::vector<unsigned int> bvh_faces(model.faces.size() / 10 * 3);
	for (int ii = 0, i = 0; ii < model.faces.size(); ii += 10, i += 3) {
		bvh_faces[i] = model.faces[ii];
		bvh_faces[i + 1] = model.faces[ii + 1];
		bvh_faces[i + 2] = model.faces[ii + 2];
	}

	std::vector<math::Vec3f> bvh_vertices(model.vertices.size());
	tex_loop(i, model.vertices.size()) {
		bvh_vertices[i][0] = model.vertices[i][0];
		bvh_vertices[i][1] = model.vertices[i][1];
		bvh_vertices[i][2] = model.vertices[i][2];
	}

	BVHTree bvh_tree(bvh_faces, bvh_vertices);
	tex_loop(j, views_num) {
		//view_direction[j] = views[j].R * Eigen::Vector3f(0.0f, 0.0f, 1.0f);
		view_direction[j] = -Eigen::Vector3f(views[j].R(2, 0),
			views[j].R(2, 1), views[j].R(2, 2));
		eigen_to_math(-views[j].R.transpose() * views[j].t, view_pos[j]);
	}
	std::vector<std::vector<Eigen::Vector2f>> views_uvs(views_num);

	tex_loop(j, views_num) {
		calculate_uvs(model, views[j], views_uvs[j]);
	}

	tex_loop(i, faces_num)
	{
		Eigen::Vector3f face_normal = get_face_normal(model, i);
		tex_loop(j, views_num)
		{
			bool visible = true;

			math::Vec3f samples[3];
			tex_loop(k, 3) {
				samples[k] = math::Vec3f(model.vertices[model.faces[i * 10 + k]][0],
					model.vertices[model.faces[i * 10 + k]][1], model.vertices[model.faces[i * 10 + k]][2]);
				BVHTree::Ray ray;
				ray.origin = samples[k];

				ray.dir = view_pos[j] - ray.origin;
				ray.tmax = ray.dir.norm();
				ray.tmin = ray.tmax * 0.0001f;
				ray.dir.normalize();

				BVHTree::Hit hit;
				if (bvh_tree.intersect(ray, &hit)) {
					visible = false;
					break;
				}
			}
			if (!visible) continue;
			if (face_normal.dot(view_direction[j]) < 0) continue;
			float face_area = get_face_area(model, views_uvs[j], i, views[j]);
			if (face_area < 1e-6) continue;
			face_areas[i][j] = face_area;
			if (max_area < face_area) max_area = face_area;
		}
	}

	tex_loop(i, faces_num) {
		tex_loop(j, views_num)
		{
			float_t normalize_face_area = face_areas[i][j];
			if (max_area > 1e-6) normalize_face_area /= max_area;
			data_costs[i][j + 1] = 1.0 - normalize_face_area;
			if (data_costs[i][j + 1] < 0.0f) data_costs[i][j + 1] = 0.0f;
			if (data_costs[i][j + 1] < 1.0f - 1e-6) {
				valid_mask[i][j] = 1;
				b_good_face[i] = 1;
			}
		}
	}
	return 0;
}